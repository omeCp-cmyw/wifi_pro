#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <termios.h>
#include <sys/select.h>

#include "esp_at.h"
#include "esp_link.h"

#define ESP_AT_BAUD      B115200
#define ESP_AT_CMD_BUF   256

/* 命令表: 模板支持vsnprintf填参, 超时毫秒/重试/成败特征 */
typedef struct {
    const char *tpl;
    int timeout_ms;
    int retries;
    const char *expect;
    const char *fail;
} wifi_at_cmd_t;

static const wifi_at_cmd_t s_cmd_tbl[WIFI_CMD_NUM] = {
    [WIFI_CMD_RST]           = { "AT+RST\r\n",                       5000, 1, "OK",   "ERROR" },
    [WIFI_CMD_AT]            = { "AT\r\n",                           5000, 2, "OK",      "ERROR" },
    [WIFI_CMD_GMR]           = { "AT+GMR\r\n",                       5000, 1, "OK",      "ERROR" },
    [WIFI_CMD_CIPMUX]        = { "AT+CIPMUX=1\r\n",                  5000, 1, "OK",      "ERROR" },
    [WIFI_CMD_CWMODE]        = { "AT+CWMODE=3\r\n",                  5000, 1, "OK",      "ERROR" },
    [WIFI_CMD_CWJAP]         = { "AT+CWJAP=\"%s\",\"%s\"\r\n",       15000, 1, "OK",      "FAIL"  },
    [WIFI_CMD_CWJAP_Q]       = { "AT+CWJAP?\r\n",                    5000, 1, "+CWJAP:", NULL    },
    [WIFI_CMD_CIPSTART_UDP]  = { "AT+CIPSTART=0,\"UDP\",\"%s\",%d\r\n", 10000, 1, "OK",  "ERROR" },
    [WIFI_CMD_CIPSTART_TCP]  = { "AT+CIPSTART=1,\"TCP\",\"%s\",%d\r\n", 15000, 1, "OK",  "ERROR" },
    [WIFI_CMD_CIPSEND]       = { "AT+CIPSEND=%d,%d\r\n",             5000, 1, ">",      "ERROR" },
    [WIFI_CMD_CIPCLOSE]      = { "AT+CIPCLOSE=%d\r\n",               5000, 0, "OK",     NULL    },
};

static int s_fd = -1;
static int s_lost;              /* 串口坏了就别再挣扎 */

static esp_at_send_cb_t s_send_cb;
static esp_at_link_cb_t s_closed_cb;
static esp_at_wifi_cb_t s_wifi_cb;

/*******************************************************************
** 函数名	: esp_at_now_ms
** 函数描述	: 单调时钟毫秒数, 所有超时判断用它
** 参数		: 无
** 返回		: 毫秒计数
********************************************************************/
uint64_t esp_at_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*******************************************************************
** 函数名	: serial_config
** 函数描述	: 配置串口为8N1原始模式, 关闭收发流控
** 参数		: [in] fd: 串口文件描述符
** 返回		: 0成功, -1失败
********************************************************************/
static int serial_config(int fd)
{
    struct termios opt;

    if (tcgetattr(fd, &opt) != 0) {
        perror("tcgetattr");
        return -1;
    }

    cfmakeraw(&opt);
    cfsetispeed(&opt, ESP_AT_BAUD);
    cfsetospeed(&opt, ESP_AT_BAUD);

    opt.c_cflag |= CLOCAL | CREAD;
    opt.c_cflag &= ~CSIZE;
    opt.c_cflag |= CS8;
    opt.c_cflag &= ~PARENB;
    opt.c_cflag &= ~CSTOPB;
    opt.c_cflag &= ~CRTSCTS;

    opt.c_iflag &= ~(IXON | IXOFF | IXANY);
    opt.c_iflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    opt.c_oflag &= ~OPOST;

    opt.c_cc[VMIN]  = 0;
    opt.c_cc[VTIME] = 0;

    tcflush(fd, TCIOFLUSH);
    if (tcsetattr(fd, TCSANOW, &opt) != 0) {
        perror("tcsetattr");
        return -1;
    }
    return 0;
}

/*******************************************************************
** 函数名	: esp_at_init
** 函数描述	: 打开并配置串口, 注册接收处理器
** 参数		: [in] dev: 串口设备路径
** 返回		: 0成功, -1失败
********************************************************************/
int esp_at_init(const char *dev)
{
    int fd = open(dev, O_RDWR | O_NOCTTY | O_NDELAY);

    if (fd < 0) {
        perror(dev);
        return -1;
    }
    /* 去掉O_NDELAY, 超时都靠select控制 */
    if (fcntl(fd, F_SETFL, 0) < 0) {
        perror("fcntl");
        close(fd);
        return -1;
    }
    if (serial_config(fd) != 0) {
        close(fd);
        return -1;
    }
    s_fd = fd;
    s_lost = 0;
    return 0;
}

/*******************************************************************
** 函数名	: esp_at_on_send_result
** 函数描述	: 注册SEND OK/SEND FAIL通知回调
** 参数		: [in] cb: 回调(参数: 链路号, 1成功0失败)
** 返回		: 无
********************************************************************/
void esp_at_on_send_result(esp_at_send_cb_t cb)
{
    s_send_cb = cb;
}

/*******************************************************************
** 函数名	: esp_at_on_link_closed
** 函数描述	: 注册"n,CLOSED"通知回调
** 参数		: [in] cb: 回调(参数: 链路号)
** 返回		: 无
********************************************************************/
void esp_at_on_link_closed(esp_at_link_cb_t cb)
{
    s_closed_cb = cb;
}

/*******************************************************************
** 函数名	: esp_at_on_wifi_disconnect
** 函数描述	: 注册WIFI DISCONNECT通知回调
** 参数		: [in] cb: 回调
** 返回		: 无
********************************************************************/
void esp_at_on_wifi_disconnect(esp_at_wifi_cb_t cb)
{
    s_wifi_cb = cb;
}

/*******************************************************************
** 函数名	: esp_at_line_hook
** 函数描述	: 文本行到达时的异步事件分发
** 参数		: [in] line: 不含换行符的行内容
** 返回		: 无
********************************************************************/
static void esp_at_line_hook(const char *line)
{
    if (strncmp(line, "SEND OK", 7) == 0) {
        if (s_send_cb)
            s_send_cb(-1, 1);
    } else if (strncmp(line, "SEND FAIL", 9) == 0) {
        if (s_send_cb)
            s_send_cb(-1, 0);
    } else if (strstr(line, "CLOSED")) {
        /* "1,CLOSED"或"closed", 多链路下带链路号前缀 */
        if (s_closed_cb && line[0] >= '0' && line[0] <= '4' && line[1] == ',')
            s_closed_cb(line[0] - '0');
    } else if (strstr(line, "WIFI DISCONNECT")) {
        if (s_wifi_cb)
            s_wifi_cb();
    }
}

/*******************************************************************
** 函数名	: esp_at_feed
** 函数描述	: 原始字节入口: 净化后回显并喂给链路层状态机
** 参数		: [in] buf: 数据缓冲
**          : [in] len: 数据长度
** 返回		: 无
********************************************************************/
static void esp_at_feed(const uint8_t *buf, int len)
{
    /* +IPD载荷是二进制, 当字符打会出乱码, 不可见字节打点;
     * 换行保留, 不然AT文本响应挤成一团 */
    printf("[RX] ");
    for (int i = 0; i < len; i++) {
        uint8_t ch = buf[i];

        if ((ch >= 0x20 && ch < 0x7f) || ch == '\r' || ch == '\n')
            putchar(ch);
        else
            putchar('.');
    }
    fflush(stdout);
    esp_link_feed(buf, len, esp_at_line_hook);
}

/*******************************************************************
** 函数名	: esp_at_pump
** 函数描述	: 单次非阻塞串口读取, 数据喂给链路层状态机
** 参数		: [in] timeout_ms: 等待数据的毫秒数, 0立即返回
** 返回		: 0正常(可能无数据), -1串口异常
********************************************************************/
int esp_at_pump(int timeout_ms)
{
    fd_set rfds;
    struct timeval tv;
    uint8_t buf[512];
    int sel_ret, got;

    if (s_fd < 0 || s_lost)
        return -1;

    FD_ZERO(&rfds);
    FD_SET(s_fd, &rfds);
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    sel_ret = select(s_fd + 1, &rfds, NULL, NULL, &tv);
    if (sel_ret < 0) {
        if (errno == EINTR)
            return 0;
        perror("select");
        s_lost = 1;
        return -1;
    }
    if (sel_ret == 0)
        return 0;

    got = read(s_fd, buf, sizeof(buf));
    if (got < 0) {
        if (errno == EINTR || errno == EAGAIN)
            return 0;
        perror("read");
        s_lost = 1;
        return -1;
    }
    if (got == 0) {
        printf("read 0, device removed?\n");
        s_lost = 1;
        return -1;
    }
    esp_at_feed(buf, got);
    return 0;
}

/*******************************************************************
** 函数名	: esp_at_pump_wait
** 函数描述	: 循环pump直到文本行中出现期望或失败特征
** 参数		: [in] expect: 期望特征串
**          : [in] fail: 失败特征串(可为NULL)
**          : [in] timeout_ms: 总超时毫秒数
** 返回		: 1等到期望, 0超时, -2等到失败特征, -1串口异常
********************************************************************/
int esp_at_pump_wait(const char *expect, const char *fail, int timeout_ms)
{
    uint64_t deadline = esp_at_now_ms() + timeout_ms;

    while (esp_at_now_ms() < deadline) {
        int left = (int)(deadline - esp_at_now_ms());

        if (esp_at_pump(left > 100 ? 100 : left) < 0)
            return -1;
        if (fail && esp_link_has_line(fail))
            return -2;
        if (esp_link_has_line(expect))
            return 1;
    }
    return 0;
}

/*******************************************************************
** 函数名	: esp_at_write_raw
** 函数描述	: 按长度发送裸字节(处理部分写与信号中断)
** 参数		: [in] data: 数据缓冲
**          : [in] len: 数据长度
** 返回		: 0成功, -1串口异常
********************************************************************/
int esp_at_write_raw(const uint8_t *data, int len)
{
    int off = 0;

    if (s_fd < 0 || s_lost)
        return -1;

    while (off < len) {
        int written = write(s_fd, data + off, len - off);

        if (written < 0) {
            if (errno == EINTR)
                continue;
            perror("write");
            s_lost = 1;
            return -1;
        }
        off += written;
    }
    printf("[TX] %d raw bytes\n", len);
    return 0;
}

/*******************************************************************
** 函数名	: esp_at_hex_dump
** 函数描述	: 报文十六进制+ASCII双栏打印, 每行16字节, 调试原始报文用
** 参数		: [in] tag: 前缀标识(如"[TX] mqtt pkt")
**          : [in] data: 数据缓冲
**          : [in] len: 数据长度
** 返回		: 无
********************************************************************/
void esp_at_hex_dump(const char *tag, const uint8_t *data, int len)
{
    printf("%s %d bytes:\n", tag, len);
    for (int off = 0; off < len; off += 16) {
        int line_len = len - off > 16 ? 16 : len - off;

        printf(" %04x ", off);
        /* 十六进制栏, 不足16字节补齐空格对整齐 */
        for (int i = 0; i < 16; i++) {
            if (i < line_len)
                printf(" %02x", data[off + i]);
            else
                printf("   ");
        }
        /* ASCII栏, 可见字符直接显示, 其余打点 */
        printf("  |");
        for (int i = 0; i < line_len; i++) {
            uint8_t ch = data[off + i];

            putchar(ch >= 0x20 && ch < 0x7f ? ch : '.');
        }
        printf("|\n");
    }
    fflush(stdout);
}

/*******************************************************************
** 函数名	: esp_at_flush
** 函数描述	: 读空串口残留数据
** 参数		: 无
** 返回		: 无
********************************************************************/
void esp_at_flush(void)
{
    for (int i = 0; i < 10; i++) {
        if (esp_at_pump(50) != 0)
            break;
        /* 没新数据就算干净了 */
        if (esp_link_pending() == 0)
            break;
    }
    /* 刚读上来的旧响应一并清掉, 免得下一条命令误匹配 */
    esp_link_clear_lines();
}

/*******************************************************************
** 函数名	: esp_at_cmd_exec
** 函数描述	: 按命令编号查表执行, 失败按表中次数重试
** 参数		: [in] id: 命令表编号
**          : [in] tpl: printf风格模板串(可为NULL, 直接用表中模板)
**          : [in] ...: 模板参数
** 返回		: ESP_AT_OK成功, 其余为错误码
********************************************************************/
int esp_at_cmd_exec(int id, const char *tpl, ...)
{
    const wifi_at_cmd_t *cmd_cfg;
    char cmd_text[ESP_AT_CMD_BUF];
    va_list arg_list;

    if (id < 0 || id >= WIFI_CMD_NUM)
        return ESP_AT_FAIL;
    cmd_cfg = &s_cmd_tbl[id];

    va_start(arg_list, tpl);
    vsnprintf(cmd_text, sizeof(cmd_text), tpl ? tpl : cmd_cfg->tpl, arg_list);
    va_end(arg_list);

    for (int attempt = 0; attempt <= cmd_cfg->retries; attempt++) {
        int wait_ret;

        if (attempt > 0) {
            printf("retry %d: %s", attempt, cmd_text);
            struct timespec ts = { 0, 500000000L };
            nanosleep(&ts, NULL);
        }
        esp_at_flush();
        if (esp_at_write_raw((const uint8_t *)cmd_text, strlen(cmd_text)) != 0)
            return ESP_AT_FATAL;

        wait_ret = esp_at_pump_wait(cmd_cfg->expect, cmd_cfg->fail,
                                    cmd_cfg->timeout_ms);
        if (wait_ret > 0)
            return ESP_AT_OK;
        if (wait_ret == -1)
            return ESP_AT_FATAL;
        if (wait_ret == -2) {
            printf("module answered %s\n", cmd_cfg->fail);
            return ESP_AT_FAIL;
        }
        printf("timeout, no '%s'\n", cmd_cfg->expect);
    }
    return ESP_AT_TIMEOUT;
}
