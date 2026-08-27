#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <signal.h>
#include <time.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/time.h>

#define SERIAL_PORT     "/dev/ttyUSB0"
#define BAUD_RATE       B115200
#define RECV_BUF_SIZE   1024

#define DEFAULT_TIMEOUT 5
#define WIFI_TIMEOUT    15
#define NTP_TIMEOUT     10

#define WIFI_SSID       "123"
#define WIFI_PASS       "yw22334455"

#define NTP_SERVER      "ntp.aliyun.com"
#define NTP_PORT        123
#define NTP_PACKET_SIZE 48
#define NTP_UNIX_DIFF   2208988800u   /* 1900->1970秒差 */
/* 时间戳合理区间, 太离谱的当垃圾丢掉 */
#define NTP_SEC_MIN     3155673600u   /* 2000年后 */
#define NTP_SEC_MAX     4260211200u   /* 2035年前 */

#define LINK_CHECK_INTERVAL 10        /* 链路检测周期(秒) */

#define LINK_MODE_SINGLE 0
#define LINK_MODE_MULTI  1
static int g_link_mode = LINK_MODE_SINGLE;

static volatile sig_atomic_t g_running = 1;

/*******************************************************************
** 函数名	: sig_handler
** 函数描述	: SIGINT/SIGTERM信号处理, 置位退出标志
** 参数		: [in] sig: 信号编号(未使用)
** 返回		: 无
********************************************************************/
static void sig_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/*******************************************************************
** 函数名	: serial_open
** 函数描述	: 打开串口设备并恢复为阻塞模式
** 参数		: [in] dev: 串口设备路径
** 返回		: 成功返回文件描述符, 失败返回-1
********************************************************************/
static int serial_open(const char *dev)
{
    int fd = open(dev, O_RDWR | O_NOCTTY | O_NDELAY);
    if (fd < 0) {
        perror("open " SERIAL_PORT);
        return -1;
    }
    /* 去掉O_NDELAY, 超时都靠后面的select控制 */
    if (fcntl(fd, F_SETFL, 0) < 0) {
        perror("fcntl");
        close(fd);
        return -1;
    }
    return fd;
}

/*******************************************************************
** 函数名	: serial_config
** 函数描述	: 配置串口为 8N1 原始模式, 关闭收发流控
** 参数		: [in] fd: 串口文件描述符
** 返回		: 成功返回0, 失败返回-1
********************************************************************/
static int serial_config(int fd)
{
    struct termios opt;

    if (tcgetattr(fd, &opt) != 0) {
        perror("tcgetattr");
        return -1;
    }

    cfmakeraw(&opt);
    cfsetispeed(&opt, BAUD_RATE);
    cfsetospeed(&opt, BAUD_RATE);

    opt.c_cflag |= CLOCAL | CREAD;
    opt.c_cflag &= ~CSIZE;
    opt.c_cflag |= CS8;             /* 8N1, 无流控 */
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
** 函数名	: serial_write
** 函数描述	: 发送字符串, 处理部分写与信号中断
** 参数		: [in] fd: 串口文件描述符
**          : [in] data: 待发送字符串(以'\0'结尾)
** 返回		: 成功返回发送字节数, 失败返回-1
********************************************************************/
static int serial_write(int fd, const char *data)
{
    int len = strlen(data);
    int off = 0;

    while (off < len) {
        int n = write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("write");
            return -1;
        }
        off += n;
    }
    printf("[TX] %s", data);
    return off;
}

/*******************************************************************
** 函数名	: serial_write_raw
** 函数描述	: 发送原始二进制数据(按长度, 不依赖'\0'结尾)
** 参数		: [in] fd: 串口文件描述符
**          : [in] data: 数据缓冲区
**          : [in] len: 数据长度
** 返回		: 成功返回发送字节数, 失败返回-1
********************************************************************/
static int serial_write_raw(int fd, const uint8_t *data, int len)
{
    int off = 0;

    while (off < len) {
        int n = write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("write");
            return -1;
        }
        off += n;
    }
    printf("[TX] %d raw bytes\n", len);
    return off;
}

/*******************************************************************
** 函数名	: serial_read_expect
** 函数描述	: 在超时时间内持续读取, 直到出现期望或失败字符串
** 参数		: [in] fd: 串口文件描述符
**          : [out] buf: 接收缓冲区
**          : [in] buf_size: 缓冲区大小
**          : [in] expect: 期望字符串(可为NULL)
**          : [in] fail: 失败特征字符串(可为NULL)
**          : [in] timeout_sec: 超时时间(秒)
** 返回		: >0收取字节数, 0超时, -1错误, -2收到失败响应
********************************************************************/
static int serial_read_expect(int fd, char *buf, int buf_size,
                              const char *expect, const char *fail,
                              int timeout_sec)
{
    static const char *empty = "";
    fd_set rfds;
    struct timeval tv;
    struct timespec t0, tnow;
    int total = 0;

    /* expect传NULL就是只等fail */
    if (expect == NULL)
        expect = empty;

    memset(buf, 0, buf_size);
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &tnow);
        double left = timeout_sec
                    - (tnow.tv_sec - t0.tv_sec)
                    - (tnow.tv_nsec - t0.tv_nsec) / 1e9;
        if (left <= 0) {
            printf("timeout, no '%s'\n", expect);
            return 0;
        }

        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec  = (long)left;
        tv.tv_usec = (long)((left - (long)left) * 1e6);

        int r = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            perror("select");
            return -1;
        }
        if (r == 0)
            continue;

        int n = read(fd, buf + total, buf_size - total - 1);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            perror("read");
            return -1;
        }
        if (n == 0) {
            printf("read 0, device removed?\n");
            return -1;
        }

        total += n;
        buf[total] = '\0';
        printf("[RX] %.*s", n, buf + total - n);
        fflush(stdout);

        if (fail && strstr(buf, fail))
            return -2;
        if (*expect && strstr(buf, expect))
            return total;
        if (total >= buf_size - 1) {
            printf("rx buffer full\n");
            break;
        }
    }
    return 0;
}

/*******************************************************************
** 函数名	: msleep
** 函数描述	: 毫秒级延时, 被信号打断后继续补足
** 参数		: [in] ms: 延时毫秒数
** 返回		: 无
********************************************************************/
static void msleep(int ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;
}

/*******************************************************************
** 函数名	: at_send_and_wait
** 函数描述	: 发送AT指令并等待期望响应, 超时后可重试
** 参数		: [in] fd: 串口文件描述符
**          : [in] cmd: AT指令字符串(含\r\n)
**          : [in] expect: 期望响应
**          : [in] fail: 失败特征字符串(可为NULL)
**          : [in] timeout_sec: 单次等待超时(秒)
**          : [in] retries: 超时重试次数
** 返回		: 0成功, -1超时/错误, -2模块返回失败
********************************************************************/
static int at_send_and_wait(int fd, const char *cmd, const char *expect,
                            const char *fail, int timeout_sec, int retries)
{
    char buf[RECV_BUF_SIZE];

    for (int i = 0; i <= retries; i++) {
        if (i > 0) {
            printf("retry %d\n", i + 1);
            msleep(500);
        }
        if (serial_write(fd, cmd) < 0)
            return -1;
        msleep(200);

        int r = serial_read_expect(fd, buf, sizeof(buf), expect, fail, timeout_sec);
        if (r > 0)
            return 0;
        if (r == -2) {
            printf("module answered %s\n", fail ? fail : "fail");
            return -2;
        }
    }
    return -1;
}

/*******************************************************************
** 函数名	: serial_flush
** 函数描述	: 读空串口接收路径上的残留数据
** 参数		: [in] fd: 串口文件描述符
** 返回		: 无
********************************************************************/
static void serial_flush(int fd)
{
    char tmp[256];
    fd_set rfds;
    struct timeval tv;

    for (;;) {
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec  = 0;
        tv.tv_usec = 100000;
        if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0)
            break;
        if (read(fd, tmp, sizeof(tmp)) <= 0)
            break;
    }
}

/*******************************************************************
** 函数名	: be32_read
** 函数描述	: 从缓冲区读取大端32位无符号整数
** 参数		: [in] p: 指向4字节数据
** 返回		: 主机序数值
********************************************************************/
static uint32_t be32_read(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

/*******************************************************************
** 函数名	: find_ntp_packet
** 函数描述	: 在混有SEND OK/+IPD前缀等杂质的字节流中定位合法的48字节NTP响应
** 参数		: [in] buf: 接收数据缓冲区
**          : [in] len: 数据长度
** 返回		: NTP包起始偏移, 未找到返回-1
********************************************************************/
static int find_ntp_packet(const uint8_t *buf, int len)
{
    for (int i = 0; i + NTP_PACKET_SIZE <= len; i++) {
        if ((buf[i] & 0x07) != 4)                 /* mode得是4(server) */
            continue;
        if (buf[i + 1] == 0 || buf[i + 1] > 15)   /* 层数 */
            continue;
        uint32_t sec = be32_read(buf + i + 40);   /* 发送时间戳 */
        if (sec < NTP_SEC_MIN || sec > NTP_SEC_MAX)
            continue;
        return i;
    }
    return -1;
}

/*******************************************************************
** 函数名	: ntp_wait_reply
** 函数描述	: 在超时时间内收集串口所有输入并扫描合法NTP响应
** 参数		: [in] fd: 串口文件描述符
**          : [out] unix_ts_out: 解析出的Unix时间戳
**          : [in] timeout_sec: 超时时间(秒)
** 返回		: 0成功, -1超时或错误
********************************************************************/
static int ntp_wait_reply(int fd, time_t *unix_ts_out, int timeout_sec)
{
    uint8_t buf[RECV_BUF_SIZE];
    fd_set rfds;
    struct timeval tv;
    struct timespec t0, tnow;
    int total = 0;

    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &tnow);
        double left = timeout_sec
                    - (tnow.tv_sec - t0.tv_sec)
                    - (tnow.tv_nsec - t0.tv_nsec) / 1e9;
        if (left <= 0) {
            printf("no ntp reply within %ds\n", timeout_sec);
            return -1;
        }

        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec  = (long)left;
        tv.tv_usec = (long)((left - (long)left) * 1e6);

        int r = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            perror("select");
            return -1;
        }
        if (r == 0)
            continue;

        int n = read(fd, buf + total, sizeof(buf) - total - 1);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            perror("read");
            return -1;
        }
        if (n == 0) {
            printf("read 0, device removed?\n");
            return -1;
        }
        total += n;

        int pos = find_ntp_packet(buf, total);
        if (pos >= 0) {
            uint32_t ntp_sec = be32_read(buf + pos + 40);
            *unix_ts_out = (time_t)(ntp_sec - NTP_UNIX_DIFF);
            printf("ntp reply at offset %d\n", pos);
            return 0;
        }

        if (total >= (int)sizeof(buf) - 1) {
            printf("buf full, reset\n");
            total = 0;
        }
    }
}

/*******************************************************************
** 函数名	: wifi_join
** 函数描述	: 连接AP并用AT+CWJAP?二次确认(部分固件不打印WIFI GOT IP)
** 参数		: [in] fd: 串口文件描述符
** 返回		: 0成功, -1失败或超时
********************************************************************/
static int wifi_join(int fd)
{
    char cmd[128];

    snprintf(cmd, sizeof(cmd),
             "AT+CWJAP_DEF=\"" WIFI_SSID "\",\"" WIFI_PASS "\"\r\n");
    if (at_send_and_wait(fd, cmd, "OK", "FAIL", WIFI_TIMEOUT, 1) != 0) {
        printf("cwjap failed or timed out\n");
        return -1;
    }
    sleep(1);

    if (at_send_and_wait(fd, "AT+CWJAP?\r\n", "+CWJAP:", NULL,
                         DEFAULT_TIMEOUT, 1) != 0) {
        printf("not associated (CWJAP?)\n");
        return -1;
    }
    return 0;
}

/*******************************************************************
** 函数名	: ntp_sync_single
** 函数描述	: 单链路透传方式做NTP同步(CIPSTART/CIPSEND/+++退出/CIPCLOSE)
** 参数		: [in] fd: 串口文件描述符
**          : [in] req: 48字节NTP请求
**          : [out] unix_ts_out: Unix时间戳
** 返回		: 0成功, -1失败
********************************************************************/
static int ntp_sync_single(int fd, const uint8_t *req, time_t *unix_ts_out)
{
    char cmd[128];

    snprintf(cmd, sizeof(cmd),
             "AT+CIPSTART=\"UDP\",\"" NTP_SERVER "\",%d\r\n", NTP_PORT);
    serial_flush(fd);
    if (at_send_and_wait(fd, cmd, "OK", "ERROR", NTP_TIMEOUT, 1) != 0) {
        printf("udp connect to %s:%d failed\n", NTP_SERVER, NTP_PORT);
        return -1;
    }
    sleep(1);

    serial_flush(fd);
    if (at_send_and_wait(fd, "AT+CIPSEND\r\n", "OK", "ERROR",
                         DEFAULT_TIMEOUT, 1) != 0) {
        printf("could not enter passthrough\n");
        return -1;
    }
    msleep(500);

    if (serial_write_raw(fd, req, NTP_PACKET_SIZE) < 0)
        return -1;

    int got = (ntp_wait_reply(fd, unix_ts_out, NTP_TIMEOUT) == 0);

    /* 退出透传: 前后各留1秒静默, +++不能带\r\n */
    sleep(1);
    if (write(fd, "+++", 3) != 3)
        perror("+++");
    sleep(1);
    serial_flush(fd);

    if (!got) {
        printf("no ntp reply\n");
        at_send_and_wait(fd, "AT+CIPCLOSE\r\n", "OK", NULL, DEFAULT_TIMEOUT, 0);
        return -1;
    }

    /* 链路可能早断了, 关不上也正常 */
    if (at_send_and_wait(fd, "AT+CIPCLOSE\r\n", "OK", NULL,
                         DEFAULT_TIMEOUT, 1) != 0)
        printf("cipclose: no OK, link probably already gone\n");
    return 0;
}

/*******************************************************************
** 函数名	: ntp_sync_multi
** 函数描述	: 多链路方式NTP同步: CIPSTART=0->CIPSEND=0,48->'>'后发载荷->CIPCLOSE=0
** 参数		: [in] fd: 串口文件描述符
**          : [in] req: 48字节NTP请求
**          : [out] unix_ts_out: Unix时间戳
** 返回		: 0成功, -1失败
********************************************************************/
static int ntp_sync_multi(int fd, const uint8_t *req, time_t *unix_ts_out)
{
    char cmd[128];

    snprintf(cmd, sizeof(cmd),
             "AT+CIPSTART=0,\"UDP\",\"" NTP_SERVER "\",%d\r\n", NTP_PORT);
    serial_flush(fd);
    if (at_send_and_wait(fd, cmd, "OK", "ERROR", NTP_TIMEOUT, 1) != 0) {
        printf("opening udp link 0 to %s:%d failed\n", NTP_SERVER, NTP_PORT);
        return -1;
    }
    sleep(1);

    serial_flush(fd);
    snprintf(cmd, sizeof(cmd), "AT+CIPSEND=0,%d\r\n", NTP_PACKET_SIZE);
    if (at_send_and_wait(fd, cmd, ">", "ERROR", DEFAULT_TIMEOUT, 1) != 0) {
        printf("link 0 cipsend failed\n");
        return -1;
    }

    if (serial_write_raw(fd, req, NTP_PACKET_SIZE) < 0)
        return -1;

    /* SEND OK和+IPD数据混在一起, 丢给同一个扫描函数处理 */
    if (ntp_wait_reply(fd, unix_ts_out, NTP_TIMEOUT) != 0) {
        printf("no ntp reply on link 0\n");
        at_send_and_wait(fd, "AT+CIPCLOSE=0\r\n", "OK", NULL,
                         DEFAULT_TIMEOUT, 0);
        return -1;
    }

    serial_flush(fd);
    if (at_send_and_wait(fd, "AT+CIPCLOSE=0\r\n", "OK", NULL,
                         DEFAULT_TIMEOUT, 1) != 0)
        printf("cipclose=0: no OK, link probably already gone\n");
    return 0;
}

/*******************************************************************
** 函数名	: ntp_sync_time
** 函数描述	: 按当前链路模式派发NTP同步并打印结果
** 参数		: [in] fd: 串口文件描述符
** 返回		: 0成功, -1失败
********************************************************************/
static int ntp_sync_time(int fd)
{
    uint8_t req[NTP_PACKET_SIZE];
    time_t unix_ts;

    memset(req, 0, sizeof(req));
    req[0] = 0x1b;  /* v3客户端 */

    int ret = (g_link_mode == LINK_MODE_MULTI)
            ? ntp_sync_multi(fd, req, &unix_ts)
            : ntp_sync_single(fd, req, &unix_ts);
    if (ret != 0)
        return ret;

    struct tm tm_utc;
    char str[32];

    gmtime_r(&unix_ts, &tm_utc);
    strftime(str, sizeof(str), "%Y-%m-%d %H:%M:%S", &tm_utc);
    printf("time utc     : %s\n", str);

    time_t local = unix_ts + 8 * 3600;
    gmtime_r(&local, &tm_utc);
    strftime(str, sizeof(str), "%Y-%m-%d %H:%M:%S", &tm_utc);
    printf("time beijing : %s\n", str);
    printf("unix epoch   : %ld\n", (long)unix_ts);
    return 0;
}

/*******************************************************************
** 函数名	: keep_long_connection
** 函数描述	: 周期性检测WiFi关联状态, 断线自动重连, 直到Ctrl+C退出
** 参数		: [in] fd: 串口文件描述符
** 返回		: 无
********************************************************************/
static void keep_long_connection(int fd)
{
    printf("link check every %ds, ctrl-c to quit\n", LINK_CHECK_INTERVAL);

    while (g_running) {
        for (int i = 0; i < LINK_CHECK_INTERVAL && g_running; i++)
            sleep(1);
        if (!g_running)
            break;

        serial_flush(fd);
        if (at_send_and_wait(fd, "AT+CWJAP?\r\n", "+CWJAP:", NULL,
                             DEFAULT_TIMEOUT, 0) == 0) {
            printf("link ok\n");
            continue;
        }

        printf("link lost, reconnecting\n");
        serial_flush(fd);
        if (wifi_join(fd) != 0)
            printf("reconnect failed, retry next cycle\n");
    }
}

/*******************************************************************
** 函数名	: run
** 函数描述	: 主流程: 复位->AT测试->模式配置->连WiFi->NTP同步->保活
** 参数		: 无
** 返回		: EXIT_SUCCESS或EXIT_FAILURE
********************************************************************/
static int run(void)
{
    int fd = serial_open(SERIAL_PORT);
    if (fd < 0)
        return EXIT_FAILURE;
    if (serial_config(fd) != 0) {
        close(fd);
        return EXIT_FAILURE;
    }
    printf("%s @115200 ready, %s link mode\n", SERIAL_PORT,
           g_link_mode == LINK_MODE_MULTI ? "multi" : "single");

    serial_flush(fd);

    /* 复位, 起来后回显默认又是开的, 先关掉 */
    at_send_and_wait(fd, "AT+RST\r\n", "ready", "ERROR", DEFAULT_TIMEOUT, 1);
    sleep(2);
    serial_flush(fd);
    at_send_and_wait(fd, "ATE0\r\n", "OK", "ERROR", 2, 1);
    serial_flush(fd);

    if (at_send_and_wait(fd, "AT\r\n", "OK", "ERROR", DEFAULT_TIMEOUT, 2) != 0) {
        printf("no AT response, check wiring\n");
        close(fd);
        return EXIT_FAILURE;
    }

    if (at_send_and_wait(fd, "AT+CWMODE=3\r\n", "OK", "ERROR",
                         DEFAULT_TIMEOUT, 1) != 0 ||
        at_send_and_wait(fd, g_link_mode == LINK_MODE_MULTI
                            ? "AT+CIPMUX=1\r\n" : "AT+CIPMODE=1\r\n",
                         "OK", "ERROR", DEFAULT_TIMEOUT, 1) != 0) {
        printf("module configuration failed\n");
        close(fd);
        return EXIT_FAILURE;
    }
    sleep(1);

    if (wifi_join(fd) != 0) {
        close(fd);
        return EXIT_FAILURE;
    }
    sleep(1);

    if (ntp_sync_time(fd) != 0) {
        printf("ntp sync failed\n");
        close(fd);
        return EXIT_FAILURE;
    }
    sleep(1);

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    keep_long_connection(fd);

    close(fd);
    return EXIT_SUCCESS;
}

/*******************************************************************
** 函数名	: usage
** 函数描述	: 打印命令行用法
** 参数		: [in] p: 程序名
** 返回		: 无
********************************************************************/
static void usage(const char *p)
{
    printf("usage: %s [0|1]   0=single link passthrough (default), 1=multi link\n", p);
}

/*******************************************************************
** 函数名	: main
** 函数描述	: 解析命令行选择单链路(0)或多链路(1)模式并执行
** 参数		: [in] argc: 参数个数
**          : [in] argv: 参数值数组
** 返回		: 程序退出码
********************************************************************/
int main(int argc, char *argv[])
{
    if (argc > 2) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (argc == 2) {
        if (strcmp(argv[1], "0") == 0)
            g_link_mode = LINK_MODE_SINGLE;
        else if (strcmp(argv[1], "1") == 0)
            g_link_mode = LINK_MODE_MULTI;
        else {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    return run();
}
