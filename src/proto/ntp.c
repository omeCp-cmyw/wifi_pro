#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ntp.h"
#include "../esp/esp_at.h"

#define NTP_UNIX_DIFF   2208988800u   /* 1900->1970秒差 */
/* 时间戳合理区间, 上限卡在2035年防uint32回卷 */
#define NTP_SEC_MIN     3155673600u   /* 2000年后 */
#define NTP_SEC_MAX     4260211200u   /* 2035年前 */

static int s_valid;
static int s_fresh;             /* 本次同步是否拿到新应答 */
static time_t s_base_unix;
static uint64_t s_base_mono_ms;

/*******************************************************************
** 函数名	: be32_read
** 函数描述	: 从缓冲区读取大端32位无符号整数
** 参数		: [in] bytes: 指向4字节数据
** 返回		: 主机序数值
********************************************************************/
static uint32_t be32_read(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16)
         | ((uint32_t)bytes[2] << 8)  | (uint32_t)bytes[3];
}

/*******************************************************************
** 函数名	: ntp_build_request
** 函数描述	: 构造48字节NTP客户端请求
** 参数		: [out] buf: 输出缓冲(至少48字节)
** 返回		: 报文长度
********************************************************************/
int ntp_build_request(uint8_t *buf)
{
    memset(buf, 0, NTP_PACKET_SIZE);
    buf[0] = 0x1b;      /* v3客户端 */
    s_fresh = 0;
    return NTP_PACKET_SIZE;
}

/*******************************************************************
** 函数名	: ntp_sync_fresh
** 函数描述	: 本次同步是否已拿到新应答(重校时等待的终止条件)
** 参数		: 无
** 返回		: 1已应答, 0还在等
********************************************************************/
int ntp_sync_fresh(void)
{
    return s_fresh;
}

/*******************************************************************
** 函数名	: ntp_parse_reply
** 函数描述	: 校验并解析48字节NTP应答, 提取发送时间戳
** 参数		: [in] data: 应答数据
**          : [in] len: 数据长度
**          : [out] unix_ts: Unix时间戳
** 返回		: 0合法, -1非法
********************************************************************/
int ntp_parse_reply(const uint8_t *data, int len, time_t *unix_ts)
{
    uint32_t sec;

    if (data == NULL || len < NTP_PACKET_SIZE)
        return -1;
    if ((data[0] & 0x07) != 4)          /* mode得是4(server) */
        return -1;
    if (data[1] == 0 || data[1] > 15)   /* 层数 */
        return -1;

    sec = be32_read(data + 40);         /* 发送时间戳 */
    if (sec < NTP_SEC_MIN || sec > NTP_SEC_MAX)
        return -1;

    *unix_ts = (time_t)(sec - NTP_UNIX_DIFF);
    return 0;
}

/*******************************************************************
** 函数名	: hex_dump
** 函数描述	: 单行十六进制打印(应答就48字节, 直接看内容)
** 参数		: [in] data: 数据
**          : [in] len: 长度
** 返回		: 无
********************************************************************/
static void hex_dump(const uint8_t *data, int len)
{
    for (int i = 0; i < len; i++)
        printf("%02x ", data[i]);
    printf("\n");
}

/*******************************************************************
** 函数名	: ntp_link_feed
** 函数描述	: 链路0载荷处理入口, 扫合法应答并同步全局时间
** 参数		: [in] link: 链路号
**          : [in] data: 载荷数据
**          : [in] len: 载荷长度
** 返回		: 无
********************************************************************/
void ntp_link_feed(int link, const uint8_t *data, int len)
{
    time_t unix_ts;

    (void)link;
    printf("ntp reply %d bytes: ", len);
    hex_dump(data, len > 64 ? 64 : len);

    /* +IPD载荷恰好是干净数据, 直接整包解析即可 */
    if (ntp_parse_reply(data, len, &unix_ts) == 0)
        ntp_set_time(unix_ts);
}

/*******************************************************************
** 函数名	: ntp_set_time
** 函数描述	: 用Unix时间戳更新全局时间基准
** 参数		: [in] unix_ts: Unix时间戳
** 返回		: 无
********************************************************************/
void ntp_set_time(time_t unix_ts)
{
    struct tm tm_utc;
    char time_str[32];
    time_t beijing_sec;

    s_base_unix = unix_ts;
    s_base_mono_ms = esp_at_now_ms();
    s_valid = 1;
    s_fresh = 1;

    gmtime_r(&unix_ts, &tm_utc);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_utc);
    printf("time utc     : %s\n", time_str);

    beijing_sec = unix_ts + 8 * 3600;
    gmtime_r(&beijing_sec, &tm_utc);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_utc);
    printf("time beijing : %s\n", time_str);
    printf("unix epoch   : %ld\n", (long)unix_ts);
}

/*******************************************************************
** 函数名	: ntp_time_valid
** 函数描述	: 查询时间是否已同步成功过
** 参数		: 无
** 返回		: 1有效, 0未同步
********************************************************************/
int ntp_time_valid(void)
{
    return s_valid;
}

/*******************************************************************
** 函数名	: ntp_now_unix
** 函数描述	: 基于同步基准+单调钟推算当前Unix时间
** 参数		: 无
** 返回		: Unix秒, 未同步返回0
********************************************************************/
time_t ntp_now_unix(void)
{
    if (!s_valid)
        return 0;
    return s_base_unix + (time_t)((esp_at_now_ms() - s_base_mono_ms) / 1000);
}

/*******************************************************************
** 函数名	: ntp_now_ms
** 函数描述	: 当前Unix毫秒时间戳(属性上报的time字段用它)
** 参数		: 无
** 返回		: 毫秒, 未同步返回0
********************************************************************/
int64_t ntp_now_ms(void)
{
    if (!s_valid)
        return 0;
    return (int64_t)s_base_unix * 1000
         + (int64_t)(esp_at_now_ms() - s_base_mono_ms);
}
