#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>

#include "esp/esp_at.h"
#include "esp/esp_link.h"
#include "esp/esp_pktq.h"
#include "proto/ntp.h"
#include "app/onenet.h"
#include "app/onenet_config.h"

static volatile sig_atomic_t g_running = 1;
static int g_wifi_down;         /* WIFI DISCONNECT置位, 先重连AP */

/*******************************************************************
** 函数名	: sig_handler
** 函数描述	: SIGINT/SIGTERM置位退出标志
** 参数		: [in] sig: 信号编号(未使用)
** 返回		: 无
********************************************************************/
static void sig_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/*******************************************************************
** 函数名	: wifi_hook
** 函数描述	: WIFI DISCONNECT异步通知入口
** 参数		: 无
** 返回		: 无
********************************************************************/
static void wifi_hook(void)
{
    g_wifi_down = 1;
    onenet_on_wifi_lost();
}

/*******************************************************************
** 函数名	: msleep
** 函数描述	: 毫秒延时, 被信号打断后补足
** 参数		: [in] ms: 毫秒数
** 返回		: 无
********************************************************************/
static void msleep(int ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;
}

/*******************************************************************
** 函数名	: at_probe
** 函数描述	: AT探活, 最多rounds轮, 每轮间隔500ms/1s
** 参数		: [in] rounds: 轮数
**          : [in] gap_ms: 轮间隔毫秒数
** 返回		: 0探通, -1无响应
********************************************************************/
static int at_probe(int rounds, int gap_ms)
{
    for (int i = 0; i < rounds; i++) {
        if (esp_at_cmd_exec(WIFI_CMD_AT, NULL) == ESP_AT_OK)
            return 0;
        msleep(gap_ms);
    }
    return -1;
}

/*******************************************************************
** 函数名	: module_init
** 函数描述	: 模块初始化序列: 探活->复位->多链路->模式->连AP
** 参数		: 无
** 返回		: 0成功, -1失败
********************************************************************/
static int module_init(void)
{
    int cmd_ret;

    /* USB转串口桥刚打开可能丢头几个字节, 先稳一下 */
    msleep(500);
    esp_at_flush();

    /* 先探活: 刚上电的模组头几条命令可能不进, 探不通说明链路有问题 */
    if (at_probe(3, 500) != 0) {
        printf("no AT response, check wiring/power/port\n");
        return -1;
    }

    /* 复位: ready可能错过(复位太快/被冲刷), 不当致命错误, 靠复位后探活确认 */
    cmd_ret = esp_at_cmd_exec(WIFI_CMD_RST, NULL);
    if (cmd_ret != ESP_AT_OK)
        printf("reset: no 'ready' (%d), continue anyway\n", cmd_ret);
    /* 复位后固件要自检一阵 */
    msleep(2000);
    esp_at_flush();

    if (at_probe(5, 1000) != 0) {
        printf("module dead after reset\n");
        return -1;
    }

    /* 固件版本仅供参考, 查不到不阻断 */
    if (esp_at_cmd_exec(WIFI_CMD_GMR, NULL) != ESP_AT_OK)
        printf("gmr failed, ignore\n");

    if (esp_at_cmd_exec(WIFI_CMD_CIPMUX, NULL) != ESP_AT_OK) {
        printf("cipmux failed\n");
        return -1;
    }
    if (esp_at_cmd_exec(WIFI_CMD_CWMODE, NULL) != ESP_AT_OK) {
        printf("cwmode failed\n");
        return -1;
    }

    cmd_ret = esp_at_cmd_exec(WIFI_CMD_CWJAP, NULL,
                              ONENET_WIFI_SSID, ONENET_WIFI_PASS);
    if (cmd_ret != ESP_AT_OK) {
        printf("join %s failed (%d)\n", ONENET_WIFI_SSID, cmd_ret);
        return -1;
    }
    /* 部分固件不打印WIFI GOT IP, 用CWJAP?二次确认 */
    msleep(1000);
    if (esp_at_cmd_exec(WIFI_CMD_CWJAP_Q, NULL) != ESP_AT_OK) {
        printf("not associated (CWJAP?)\n");
        return -1;
    }
    printf("wifi joined: %s\n", ONENET_WIFI_SSID);
    /* 复位/重连过程中的WIFI DISCONNECT属于预期噪声, 清掉 */
    g_wifi_down = 0;
    return 0;
}

/*******************************************************************
** 函数名	: ntp_sync
** 函数描述	: 链路0校时: 开UDP->发请求->等应答->关链路
** 参数		: 无
** 返回		: 0同步成功, -1失败(不阻断后续流程)
********************************************************************/
static int ntp_sync(void)
{
    uint8_t req[NTP_PACKET_SIZE];
    int req_len, try_idx, sync_ok = 0;

    esp_at_flush();
    if (esp_at_cmd_exec(WIFI_CMD_CIPSTART_UDP, NULL,
                        ONENET_NTP_SERVER, ONENET_NTP_PORT) != ESP_AT_OK) {
        printf("open udp link 0 failed\n");
        return -1;
    }

    req_len = ntp_build_request(req);
    for (try_idx = 0; try_idx < ONENET_NTP_TRIES && g_running; try_idx++) {
        uint64_t deadline;

        esp_at_flush();
        if (esp_at_cmd_exec(WIFI_CMD_CIPSEND, NULL,
                            ONENET_LINK_NTP, req_len) != ESP_AT_OK) {
            printf("cipsend link 0 failed\n");
            break;
        }
        esp_at_hex_dump("[TX] ntp request", req, req_len);
        if (esp_at_write_raw(req, req_len) != 0)
            break;

        deadline = esp_at_now_ms() + ONENET_NTP_TIMEOUT_MS;
        while (!ntp_sync_fresh() && esp_at_now_ms() < deadline) {
            if (esp_at_pump(100) < 0)
                break;
        }
        if (ntp_sync_fresh()) {
            sync_ok = 1;
            break;
        }
        printf("no ntp reply within %dms\n", ONENET_NTP_TIMEOUT_MS);
    }

    /* 不管成败都释放链路0, 别占着 */
    esp_at_cmd_exec(WIFI_CMD_CIPCLOSE, NULL, ONENET_LINK_NTP);
    if (!sync_ok)
        printf("ntp sync failed, continue without valid time\n");
    return sync_ok ? 0 : -1;
}

/*******************************************************************
** 函数名	: onenet_reconnect
** 函数描述	: 链路1掉线后的恢复: 必要时重连AP/补校时, 指数退避重建;
**             补校时连败达上限后改用兜底过期时间建链
** 参数		: [in] backoff: 上次退避秒数
** 返回		: 本次退避秒数(成功后为0)
********************************************************************/
static int onenet_reconnect(int backoff)
{
    static int ntp_retry_cnt;
    uint64_t retry_at = esp_at_now_ms();

    if (backoff == 0)
        backoff = ONENET_RECONN_MIN_SEC;

    while (g_running && !onenet_is_online()) {
        int wait_ms = 200;

        if (esp_at_pump(wait_ms) < 0)
            return 0;           /* 串口没了直接让main退出 */
        esp_pktq_pump();

        if (esp_at_now_ms() < retry_at)
            continue;

        if (g_wifi_down) {
            if (module_init() != 0) {
                printf("rejoin failed, retry later\n");
                retry_at = esp_at_now_ms() + (uint64_t)backoff * 1000;
                backoff = backoff * 2 > ONENET_RECONN_MAX_SEC
                        ? ONENET_RECONN_MAX_SEC : backoff * 2;
                continue;
            }
            g_wifi_down = 0;
        }

        /* token的et依赖NTP时间戳, 未校时先补校时; 连败达上限后放行兜底时间戳 */
        if (!ntp_time_valid()) {
            if (ntp_sync() == 0) {
                printf("ntp synced\n");
                ntp_retry_cnt = 0;
            } else if (ntp_retry_cnt < ONENET_NTP_RECONN_TRIES - 1) {
                ntp_retry_cnt++;
                printf("ntp sync failed, retry %d/%d\n",
                       ntp_retry_cnt, ONENET_NTP_RECONN_TRIES);
                retry_at = esp_at_now_ms() + (uint64_t)backoff * 1000;
                backoff = backoff * 2 > ONENET_RECONN_MAX_SEC
                        ? ONENET_RECONN_MAX_SEC : backoff * 2;
                continue;
            } else {
                printf("ntp sync failed %d times, build with fallback et\n",
                       ONENET_NTP_RECONN_TRIES);
            }
        }

        printf("connecting onenet, backoff %ds\n", backoff);
        if (onenet_connect(!ntp_time_valid()) == 0) {
            ntp_retry_cnt = 0;
            return 0;
        }

        retry_at = esp_at_now_ms() + (uint64_t)backoff * 1000;
        backoff = backoff * 2 > ONENET_RECONN_MAX_SEC
                ? ONENET_RECONN_MAX_SEC : backoff * 2;
    }
    return g_running ? backoff : 0;
}

/*******************************************************************
** 函数名	: run
** 函数描述	: 主流程: 初始化->链路0校时->链路1建链->主循环
** 参数		: 无
** 返回		: 程序退出码
********************************************************************/
static int run(void)
{
    int backoff = 0;
    uint64_t last_resync = 0, last_ap_check = 0;

    if (esp_at_init(ONENET_SERIAL_DEV) != 0)
        return EXIT_FAILURE;
    esp_link_init();
    esp_pktq_init();
    esp_link_register(ONENET_LINK_NTP, ntp_link_feed);
    esp_link_register(ONENET_LINK_MQTT, onenet_link_feed);
    esp_at_on_link_closed(onenet_on_link_closed);
    esp_at_on_wifi_disconnect(wifi_hook);
    onenet_init();

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    printf("%s @115200, multi link: 0=NTP udp, 1=OneNET tcp\n",
           ONENET_SERIAL_DEV);

    if (module_init() != 0)
        return EXIT_FAILURE;

    /* 阶段一: 链路0校时, 成功或重试耗尽才放行 */
    ntp_sync();
    last_resync = esp_at_now_ms();

    /* 阶段二: 链路1建链, 首次不允许兜底时间, 失败交重连流程补校时 */
    if (onenet_connect(0) != 0)
        printf("onenet connect failed, enter reconnect loop\n");

    printf("main loop running, ctrl-c to quit\n");
    while (g_running) {
        uint64_t now;

        if (esp_at_pump(200) < 0) {
            printf("serial lost, exit\n");
            break;
        }
        now = esp_at_now_ms();
        esp_pktq_pump();
        onenet_process();

        /* 云端掉线走退避重连 */
        if (!onenet_is_online()) {
            backoff = onenet_reconnect(backoff);
            last_ap_check = esp_at_now_ms();
            continue;
        }
        backoff = 0;

        /* 周期性重校时, 只动链路0 */
        if (now - last_resync >= (uint64_t)ONENET_NTP_RESYNC_SEC * 1000) {
            ntp_sync();
            last_resync = esp_at_now_ms();
        }

        /* AP关联检测, 掉线交给重连流程 */
        if (!g_wifi_down &&
            now - last_ap_check >= (uint64_t)ONENET_AP_CHECK_SEC * 1000) {
            if (esp_at_cmd_exec(WIFI_CMD_CWJAP_Q, NULL) != ESP_AT_OK) {
                printf("ap association lost\n");
                g_wifi_down = 1;
                onenet_on_wifi_lost();
            }
            last_ap_check = esp_at_now_ms();
        }
    }

    /* 退出前收尾: 在线就关掉云端链路 */
    if (onenet_is_online())
        esp_at_cmd_exec(WIFI_CMD_CIPCLOSE, NULL, ONENET_LINK_MQTT);
    return EXIT_SUCCESS;
}

/*******************************************************************
** 函数名	: main
** 函数描述	: 程序入口
** 参数		: 无
** 返回		: 程序退出码
********************************************************************/
int main(void)
{
    return run();
}
