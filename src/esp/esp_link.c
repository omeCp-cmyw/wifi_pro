#include <stdio.h>
#include <string.h>

#include "esp_link.h"
#include "esp_at.h"

/*
 * 接收侧只有两件事: 从原始字节流里切出文本行, 切出+IPD载荷。
 * +IPD,<link>,<len>: 后面紧跟len字节二进制, 中间可能混着回显,
 * 也可能一个包跨好几次read到达, 所以只能按字节流状态机来。
 */

/* 原始缓冲区, 未识别的字节都先堆这 */
static uint8_t s_raw[ESP_LINK_RXBUF];
static int s_raw_len;

/* 文本行区, has_line在这上面找特征 */
static char s_line_buf[ESP_LINK_RXBUF];
static int s_line_len;

static esp_link_rx_cb_t s_rx_cb[ESP_LINK_MAX];

/*******************************************************************
** 函数名	: esp_link_init
** 函数描述	: 清空接收状态机与注册表
** 参数		: 无
** 返回		: 无
********************************************************************/
void esp_link_init(void)
{
    s_raw_len = 0;
    s_line_len = 0;
    memset(s_rx_cb, 0, sizeof(s_rx_cb));
}

/*******************************************************************
** 函数名	: esp_link_register
** 函数描述	: 注册某条链路的数据处理器, 仿YX_S32K_Register
** 参数		: [in] link: 链路号(0~4)
**          : [in] cb: 载荷到达回调
** 返回		: 0成功, -1链路号非法或已注册
********************************************************************/
int esp_link_register(int link, esp_link_rx_cb_t cb)
{
    if (link < 0 || link >= ESP_LINK_MAX || cb == NULL)
        return -1;
    if (s_rx_cb[link] != NULL)
        return -1;
    s_rx_cb[link] = cb;
    return 0;
}

/*******************************************************************
** 函数名	: raw_consume
** 函数描述	: 从原始缓冲区头部移走n字节
** 参数		: [in] n: 移走的字节数
** 返回		: 无
********************************************************************/
static void raw_consume(int n)
{
    if (n <= 0)
        return;
    if (n >= s_raw_len) {
        s_raw_len = 0;
        return;
    }
    memmove(s_raw, s_raw + n, s_raw_len - n);
    s_raw_len -= n;
}

/*******************************************************************
** 函数名	: line_append
** 函数描述	: 把文本字节追加到行缓冲区(供has_line检索)
** 参数		: [in] data: 文本字节
**          : [in] len: 长度
** 返回		: 无
********************************************************************/
static void line_append(const uint8_t *data, int len)
{
    int room = (int)sizeof(s_line_buf) - s_line_len - 1;

    if (len > room)
        len = room;
    if (len <= 0)
        return;
    /* 启动乱码可能带0x00, 直接拷进来会把行缓冲截断, 后面的
     * OK/ERROR就再也匹配不上了, 不可见字符一律替换成空格 */
    for (int i = 0; i < len; i++) {
        uint8_t ch = data[i];

        s_line_buf[s_line_len++] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : ' ';
    }
    s_line_buf[s_line_len] = '\0';
}

/*******************************************************************
** 函数名	: line_deliver
** 函数描述	: 把一行完整文本(去掉CR/LF)交给回调
** 参数		: [in] start: 行首在原始缓冲区的偏移
**          : [in] end: 换行符偏移
**          : [in] line_cb: 行回调
** 返回		: 无
********************************************************************/
static void line_deliver(int start, int end, esp_link_line_cb_t line_cb)
{
    char line[256];
    int len = end - start;

    if (len <= 0)
        return;
    if (len > (int)sizeof(line) - 1)
        len = (int)sizeof(line) - 1;
    memcpy(line, s_raw + start, len);
    line[len] = '\0';

    /* 去掉尾部CR */
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n'))
        line[--len] = '\0';

    line_append((const uint8_t *)s_raw + start, end - start + 1);
    if (len > 0 && line_cb)
        line_cb(line);
}

/*******************************************************************
** 函数名	: line_extract
** 函数描述	: 在+IPD头出现之前, 把能凑齐的文本行逐行切出
** 参数		: [in] stop: 扫描截止偏移(+IPD头位置或缓冲区末尾)
**          : [in] line_cb: 行回调
** 返回		: 实际消费的字节数
********************************************************************/
static int line_extract(int stop, esp_link_line_cb_t line_cb)
{
    int pos = 0;
    int line_start = 0;

    while (pos < stop) {
        if (s_raw[pos] == '\n') {
            line_deliver(line_start, pos, line_cb);
            line_start = pos + 1;
        }
        pos++;
    }
    if (line_start > 0)
        return line_start;

    /* CIPSEND的提示符"> "不带换行, 模块发完就闭嘴等数据,
     * 不当一行处理的话永远切不出来, 只能死等超时 */
    if (stop >= 2 && stop <= 4 &&
        s_raw[stop - 2] == '>' && s_raw[stop - 1] == ' ') {
        line_deliver(0, stop - 1, line_cb);
        return stop;
    }
    return 0;
}

/*******************************************************************
** 函数名	: ipd_parse_head
** 函数描述	: 解析"+IPD,<link>,<len>:"头, 头不全时等下次数据
** 参数		: [in] p: +IPD起始处
**          : [in] avail: 可用字节数
**          : [out] link: 链路号
**          : [out] dlen: 载荷长度
**          : [out] used: 头部总字节数
** 返回		: 0头完整, -1还要等数据, -2格式非法(丢弃该头)
********************************************************************/
static int ipd_parse_head(const uint8_t *buf, int avail,
                          int *link, int *dlen, int *used)
{
    int pos = 4;  /* 跳过"+IPD" */
    int len_val;

    if (avail < 9)            /* "+IPD,0,1:"最短9字节 */
        return -1;
    if (buf[pos] != ',')
        return -2;
    pos++;
    if (buf[pos] < '0' || buf[pos] > '4')
        return -2;
    *link = buf[pos] - '0';
    pos++;
    if (pos >= avail)
        return -1;
    if (buf[pos] != ',')
        return -2;
    pos++;

    len_val = 0;
    while (pos < avail && buf[pos] >= '0' && buf[pos] <= '9') {
        len_val = len_val * 10 + (buf[pos] - '0');
        if (len_val > 8192)     /* ESP01S单包到不了这么大, 防脏数据 */
            return -2;
        pos++;
    }
    if (pos >= avail)
        return -1;              /* 还没看到冒号 */
    if (buf[pos] != ':')
        return -2;
    *dlen = len_val;
    *used = pos + 1;
    return 0;
}

/*******************************************************************
** 函数名	: ipd_dispatch
** 函数描述	: +IPD载荷按链路号分发, 超长分段投递
** 参数		: [in] link: 链路号
**          : [in] data: 载荷数据
**          : [in] len: 载荷长度
** 返回		: 无
********************************************************************/
static void ipd_dispatch(int link, const uint8_t *data, int len)
{
    if (link < 0 || link >= ESP_LINK_MAX || s_rx_cb[link] == NULL) {
        printf("ipd on unregistered link %d, %d bytes dropped\n", link, len);
        return;
    }
    printf("[RX] ipd link %d, %d bytes\n", link, len);
    esp_at_hex_dump("[RX] ipd payload", data, len);
    s_rx_cb[link](link, data, len);
}

/*******************************************************************
** 函数名	: ipd_try_take
** 函数描述	: 缓冲区头部若是完整+IPD包就消费掉并分发
** 参数		: [in] line_cb: 行回调(包前残余文本还要走行处理)
** 返回		: 消费的字节数, 0表示头部不是完整+IPD包
********************************************************************/
static int ipd_try_take(esp_link_line_cb_t line_cb)
{
    int link, dlen, used;
    int head_state;

    if (s_raw_len < 9 || memcmp(s_raw, "+IPD,", 5) != 0)
        return 0;

    head_state = ipd_parse_head(s_raw, s_raw_len, &link, &dlen, &used);
    if (head_state == -2) {
        /* 格式非法, 把头当文本处理掉一个字节再扫 */
        line_append(s_raw, 1);
        (void)line_cb;
        raw_consume(1);
        return 1;
    }
    if (head_state == -1)
        return 0;               /* 头没到齐, 等数据 */

    if (s_raw_len < used + dlen)
        return 0;             /* 载荷还没到齐 */

    ipd_dispatch(link, s_raw + used, dlen);
    raw_consume(used + dlen);
    return used + dlen;
}

/*******************************************************************
** 函数名	: esp_link_feed
** 函数描述	: 喂入原始字节, 状态机拆出文本行与+IPD载荷
** 参数		: [in] buf: 原始数据
**          : [in] len: 数据长度
**          : [in] line_cb: 文本行回调(可为NULL)
** 返回		: 无
********************************************************************/
void esp_link_feed(const uint8_t *buf, int len, esp_link_line_cb_t line_cb)
{
    int room = (int)sizeof(s_raw) - s_raw_len;

    if (len > room) {
        /* 缓冲区塞不下只能丢老的, +IPD会解析失败重来 */
        raw_consume(len - room);
    }
    memcpy(s_raw + s_raw_len, buf, len);
    s_raw_len += len;

    for (;;) {
        uint8_t *ipd_pos;
        int stop, consumed;

        if (s_raw_len == 0)
            break;

        if (memcmp(s_raw, "+IPD,", 5) == 0) {
            /* 头部就是+IPD, 直接尝试取包 */
            if (ipd_try_take(line_cb) > 0)
                continue;
            break;
        }

        /* 找下一个+IPD头, 之前的部分按文本行切 */
        ipd_pos = memchr(s_raw, '+', s_raw_len);
        while (ipd_pos && s_raw_len - (ipd_pos - s_raw) >= 5 &&
               memcmp(ipd_pos, "+IPD,", 5) != 0)
            ipd_pos = memchr(ipd_pos + 1, '+',
                             s_raw_len - (ipd_pos + 1 - s_raw));

        stop = ipd_pos ? (int)(ipd_pos - s_raw) : s_raw_len;
        consumed = line_extract(stop, line_cb);
        if (consumed > 0) {
            raw_consume(consumed);
            continue;
        }

        /* 没有完整行可切 */
        if (ipd_pos && ipd_pos == s_raw) {
            /* 行没凑齐但+IPD就在开头, 尝试取包 */
            if (ipd_try_take(line_cb) > 0)
                continue;
            break;
        }
        if (!ipd_pos && stop >= (int)sizeof(s_raw) / 2) {
            /* 攒了半缓冲还没换行, 全当文本行扔掉防憋死 */
            line_deliver(0, stop, line_cb);
            raw_consume(stop);
            continue;
        }
        break;
    }
}

/*******************************************************************
** 函数名	: esp_link_has_line
** 函数描述	: 查询最近文本区中是否出现过特征串, 命中后自动清空
** 参数		: [in] feature: 特征串
** 返回		: 1命中, 0未命中
********************************************************************/
int esp_link_has_line(const char *feature)
{
    if (feature == NULL || *feature == '\0')
        return 0;
    if (strstr(s_line_buf, feature) == NULL)
        return 0;
    /* 命中即清空, 同一条响应不能被匹配两次 */
    esp_link_clear_lines();
    return 1;
}

/*******************************************************************
** 函数名	: esp_link_clear_lines
** 函数描述	: 清空文本行缓冲区, 冲刷旧响应时用防误匹配
** 参数		: 无
** 返回		: 无
********************************************************************/
void esp_link_clear_lines(void)
{
    s_line_len = 0;
    s_line_buf[0] = '\0';
}

/*******************************************************************
** 函数名	: esp_link_pending
** 函数描述	: 原始缓冲区未处理的字节数
** 参数		: 无
** 返回		: 字节数
********************************************************************/
int esp_link_pending(void)
{
    return s_raw_len;
}
