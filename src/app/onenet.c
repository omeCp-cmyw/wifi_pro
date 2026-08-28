#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "onenet.h"
#include "onenet_config.h"
#include "onenet_token.h"
#include "../esp/esp_at.h"
#include "../esp/esp_pktq.h"
#include "../proto/mqtt.h"
#include "../proto/ntp.h"

enum {
    PROP_INT,
    PROP_FLOAT,
    PROP_BOOL
};

typedef struct {
    const char *key;
    int type;
    int writable;
    int int_val;        /* PROP_INT的当前值 */
    float float_val;    /* PROP_FLOAT的当前值 */
    int bool_val;       /* PROP_BOOL的当前值 */
} prop_t;

/* 与onenet_mqtt工程的物模型保持一致 */
static prop_t s_props[] = {
    { "CSQ",               PROP_INT,   0, 20,  0.0f,   0 },
    { "battery_percentage", PROP_INT,  0, 90,  0.0f,   0 },
    { "battery_state",     PROP_INT,   0, 2,   0.0f,   0 },
    { "hot_valve",         PROP_INT,   1, 10,  0.0f,   0 },
    { "humidity_value",    PROP_INT,   0, 45,  0.0f,   0 },
    { "maxhum_set",        PROP_INT,   1, 80,  0.0f,   0 },
    { "maxtemp_set",       PROP_FLOAT, 1, 0,   60.0f,  0 },
    { "minihum_set",       PROP_INT,   1, 20,  0.0f,   0 },
    { "minitemp_set",      PROP_FLOAT, 1, 0,   -10.0f, 0 },
    { "temp_unit_convert", PROP_BOOL,  1, 0,   0.0f,   0 },
    { "temp_value",        PROP_FLOAT, 0, 0,   25.5f,  0 },
};
#define PROP_NUM   (sizeof(s_props) / sizeof(s_props[0]))

static int g_msg_id = 1;
static char g_platform_id[32] = "0";    /* 平台下发命令的id, 回执时带上 */

/* led事件模拟源 */
static int g_led1, g_led2, g_led_dirty;

/* 建链状态 */
enum { SUB_IDLE, SUB_RUNNING, SUB_DONE };
static int s_online;
static int s_sub_state = SUB_IDLE;
static int s_suback_cnt;
static int s_connack_ok;
static int s_ping_pending;
static uint64_t s_last_report;
static uint64_t s_last_ping;
static uint64_t s_last_led_sim;

/* 订阅的4个下行topic模板 */
static const char *s_sub_topics[] = {
    ONENET_TOPIC_POST_REPLY,
    ONENET_TOPIC_EVENT_REPLY,
    ONENET_TOPIC_SET,
    ONENET_TOPIC_DESIRED_REPLY,
};
#define SUB_TOPIC_NUM (sizeof(s_sub_topics) / sizeof(s_sub_topics[0]))

/* 预拼好的下行topic, 收消息时直接strcmp */
static char s_topic_post_reply[ONENET_TOPIC_MAX_LEN];
static char s_topic_event_reply[ONENET_TOPIC_MAX_LEN];
static char s_topic_set[ONENET_TOPIC_MAX_LEN];
static char s_topic_desired_reply[ONENET_TOPIC_MAX_LEN];

/*******************************************************************
** 函数名	: json_append
** 函数描述	: 往缓冲区追加格式化文本, 越界自动截断
** 参数		: [in] buf: 目标缓冲
**          : [in] size: 缓冲大小
**          : [in] off: 当前偏移
**          : [in] fmt: 格式串
** 返回		: 新偏移
********************************************************************/
static int json_append(char *buf, int size, int off, const char *fmt, ...)
{
    va_list arg_list;
    int written;

    if (off < 0 || off >= size - 1)
        return off;
    va_start(arg_list, fmt);
    written = vsnprintf(buf + off, size - off, fmt, arg_list);
    va_end(arg_list);
    if (written < 0)
        return off;
    off += written;
    if (off >= size)
        off = size - 1;
    return off;
}

/*******************************************************************
** 函数名	: prop_value_str
** 函数描述	: 属性值转JSON文本(数字不加引号, bool小写)
** 参数		: [in] prop: 属性项
**          : [out] buf: 输出缓冲
**          : [in] size: 缓冲大小
** 返回		: 写入长度
********************************************************************/
static int prop_value_str(const prop_t *prop, char *buf, int size)
{
    switch (prop->type) {
    case PROP_BOOL:
        return snprintf(buf, size, "%s", prop->bool_val ? "true" : "false");
    case PROP_FLOAT:
        return snprintf(buf, size, "%.1f", prop->float_val);
    default:
        return snprintf(buf, size, "%d", prop->int_val);
    }
}

/*******************************************************************
** 函数名	: build_property_post
** 函数描述	: 构造属性上报JSON(11个属性全量, 含value+time)
** 参数		: [out] buf: 输出缓冲
**          : [in] size: 缓冲大小
** 返回		: JSON长度, -1失败
********************************************************************/
static int build_property_post(char *buf, int size)
{
    int64_t ts = ntp_now_ms();
    int off = 0;
    char vstr[32];

    off = json_append(buf, size, off,
                      "{\"id\":\"%d\",\"version\":\"1.0\",\"params\":{",
                      g_msg_id++);
    for (size_t i = 0; i < PROP_NUM; i++) {
        prop_value_str(&s_props[i], vstr, sizeof(vstr));
        off = json_append(buf, size, off, "%s\"%s\":{\"value\":%s,\"time\":%lld}",
                          i ? "," : "", s_props[i].key, vstr, (long long)ts);
    }
    off = json_append(buf, size, off, "}}");
    return off > 0 && off < size ? off : -1;
}

/*******************************************************************
** 函数名	: build_event_led
** 函数描述	: 构造led事件上报JSON
** 参数		: [out] buf: 输出缓冲
**          : [in] size: 缓冲大小
** 返回		: JSON长度, -1失败
********************************************************************/
static int build_event_led(char *buf, int size)
{
    int64_t ts = ntp_now_ms();
    int off;

    off = json_append(buf, size, 0,
        "{\"id\":\"%d\",\"version\":\"1.0\",\"params\":"
        "{\"led\":{\"value\":{\"led1\":%s,\"led2\":%s},\"time\":%lld}}}",
        g_msg_id++, g_led1 ? "true" : "false", g_led2 ? "true" : "false",
        (long long)ts);
    return off > 0 && off < size ? off : -1;
}

/*******************************************************************
** 函数名	: build_set_reply
** 函数描述	: 构造属性设置回执JSON, id沿用平台下发命令的id
** 参数		: [out] buf: 输出缓冲
**          : [in] size: 缓冲大小
**          : [in] code: 结果码(200/400)
** 返回		: JSON长度, -1失败
********************************************************************/
static int build_set_reply(char *buf, int size, int code)
{
    int off;

    off = json_append(buf, size, 0,
                      "{\"id\":\"%s\",\"code\":%d,\"msg\":\"%s\"}",
                      g_platform_id, code, code == 200 ? "success" : "failed");
    return off > 0 && off < size ? off : -1;
}

/*******************************************************************
** 函数名	: build_desired_get
** 函数描述	: 构造获取期望值请求JSON, 平台要求params为标识符数组,
**          : 不填或给对象格式都会被拒收(2410或code6)
** 参数		: [out] buf: 输出缓冲
**          : [in] size: 缓冲大小
** 返回		: JSON长度, -1失败
********************************************************************/
static int build_desired_get(char *buf, int size)
{
    int off;
    int first = 1;

    off = json_append(buf, size, 0, "{\"id\":\"%d\",\"version\":\"1.0\","
                      "\"params\":[", g_msg_id++);
    for (size_t i = 0; i < PROP_NUM; i++) {
        if (!s_props[i].writable)
            continue;
        off = json_append(buf, size, off, "%s\"%s\"",
                          first ? "" : ",", s_props[i].key);
        first = 0;
    }
    off = json_append(buf, size, off, "]}");
    return off > 0 && off < size ? off : -1;
}

/*******************************************************************
** 函数名	: json_find_key
** 函数描述	: 定位"key":的位置, 返回值指向冒号后
** 参数		: [in] json: JSON文本
**          : [in] key: 字段名
** 返回		: 冒号后的位置, 未找到返回NULL
********************************************************************/
static const char *json_find_key(const char *json, const char *key)
{
    char pat[64];
    const char *pos = json;
    int pat_len;

    snprintf(pat, sizeof(pat), "\"%s\"", key);
    pat_len = (int)strlen(pat);
    while ((pos = strstr(pos, pat)) != NULL) {
        const char *after_key = pos + pat_len;

        while (*after_key == ' ' || *after_key == '\t')
            after_key++;
        if (*after_key == ':')
            return after_key + 1;
        pos += pat_len;
    }
    return NULL;
}

/*******************************************************************
** 函数名	: json_get_number
** 函数描述	: 提取数值字段, 兼容数字与带引号的数字
** 参数		: [in] json: JSON文本
**          : [in] key: 字段名
**          : [out] out: 数值
** 返回		: 0成功, -1未找到
********************************************************************/
static int json_get_number(const char *json, const char *key, double *out)
{
    const char *pos = json_find_key(json, key);

    if (pos == NULL)
        return -1;
    while (*pos == ' ')
        pos++;
    if (*pos == '"')
        pos++;
    if (*pos == 't') {            /* true当1 */
        *out = 1;
        return 0;
    }
    if (*pos == 'f') {            /* false当0 */
        *out = 0;
        return 0;
    }
    *out = strtod(pos, NULL);
    return 0;
}

/*******************************************************************
** 函数名	: json_get_string
** 函数描述	: 提取字符串字段
** 参数		: [in] json: JSON文本
**          : [in] key: 字段名
**          : [out] out: 输出缓冲
**          : [in] out_size: 缓冲大小
** 返回		: 0成功, -1未找到
********************************************************************/
static int json_get_string(const char *json, const char *key,
                           char *out, int out_size)
{
    const char *pos = json_find_key(json, key);
    int copied = 0;

    if (pos == NULL)
        return -1;
    while (*pos == ' ')
        pos++;
    if (*pos != '"')
        return -1;
    pos++;
    while (*pos && *pos != '"' && copied < out_size - 1)
        out[copied++] = *pos++;
    out[copied] = '\0';
    return 0;
}

/*******************************************************************
** 函数名	: parse_reply_json
** 函数描述	: 解析平台回执, 兼容errcode/code与errmsg/msg两种字段名
** 参数		: [in] json: 回执JSON
**          : [out] err_code: 错误码
**          : [out] err_msg: 错误描述
**          : [in] msg_size: 描述缓冲大小
** 返回		: 0成功, -1失败
********************************************************************/
static int parse_reply_json(const char *json, int *err_code,
                            char *err_msg, int msg_size)
{
    double val;

    *err_code = -1;
    err_msg[0] = '\0';

    /* 平台回执的id若更大, 本地计数器跟上, 避免id乱跳 */
    if (json_get_number(json, "id", &val) == 0 && (int)val >= g_msg_id)
        g_msg_id = (int)val + 1;

    if (json_get_number(json, "errcode", &val) != 0 &&
        json_get_number(json, "code", &val) != 0)
        return -1;
    *err_code = (int)val;

    if (json_get_string(json, "errmsg", err_msg, msg_size) != 0)
        json_get_string(json, "msg", err_msg, msg_size);
    return 0;
}

/*******************************************************************
** 函数名	: prop_apply
** 函数描述	: 把数值写入属性表(按类型分派)
** 参数		: [in] prop: 属性项
**          : [in] value: 数值
** 返回		: 无
********************************************************************/
static void prop_apply(prop_t *prop, double value)
{
    switch (prop->type) {
    case PROP_BOOL:
        prop->bool_val = value != 0;
        printf("prop %s <- %s\n", prop->key,
               prop->bool_val ? "true" : "false");
        break;
    case PROP_FLOAT:
        prop->float_val = (float)value;
        printf("prop %s <- %.1f\n", prop->key, prop->float_val);
        break;
    default:
        prop->int_val = (int)value;
        printf("prop %s <- %d\n", prop->key, prop->int_val);
        break;
    }
}

/*******************************************************************
** 函数名	: parse_set_json
** 函数描述	: 解析平台属性设置, 平台格式为params内直接值
** 参数		: [in] json: 设置命令JSON
** 返回		: 0有属性被设置, -1无有效属性
********************************************************************/
static int parse_set_json(const char *json)
{
    int applied = 0;

    json_get_string(json, "id", g_platform_id, sizeof(g_platform_id));

    for (size_t i = 0; i < PROP_NUM; i++) {
        double set_val;

        if (!s_props[i].writable)
            continue;
        if (json_get_number(json, s_props[i].key, &set_val) == 0) {
            prop_apply(&s_props[i], set_val);
            applied++;
        }
    }
    return applied > 0 ? 0 : -1;
}

/*******************************************************************
** 函数名	: json_get_nested_number
** 函数描述	: 提取"key":{..."value":v形式的嵌套数值(期望值回包格式)
** 参数		: [in] json: JSON文本
**          : [in] key: 属性名
**          : [out] out: 数值
** 返回		: 0成功, -1未找到
********************************************************************/
static int json_get_nested_number(const char *json, const char *key,
                                  double *out)
{
    char pat[64];
    const char *pos;

    snprintf(pat, sizeof(pat), "\"%s\":{", key);
    pos = strstr(json, pat);
    if (pos == NULL) {
        /* 有的回包直接给裸值 */
        snprintf(pat, sizeof(pat), "\"%s\"", key);
        pos = strstr(json, pat);
        if (pos == NULL)
            return -1;
    }
    pos = strstr(pos, "\"value\"");
    if (pos == NULL)
        return -1;
    pos = strchr(pos, ':');
    if (pos == NULL)
        return -1;
    pos++;
    while (*pos == ' ')
        pos++;
    if (*pos == 't') {
        *out = 1;
        return 0;
    }
    if (*pos == 'f') {
        *out = 0;
        return 0;
    }
    if (*pos == '"')
        pos++;
    *out = strtod(pos, NULL);
    return 0;
}

/*******************************************************************
** 函数名	: parse_desired_reply
** 函数描述	: 解析期望值回包, 把各可写属性的期望值应用到本地
** 参数		: [in] json: 回包JSON
** 返回		: 0有属性被应用, -1无有效内容
********************************************************************/
static int parse_desired_reply(const char *json)
{
    double code;
    int applied = 0;

    if (json_get_number(json, "code", &code) == 0 &&
        (int)code != 0 && (int)code != 200) {
        printf("desired get refused, code=%d\n", (int)code);
        return -1;
    }

    /* 期望值在data字段里, 先定位再找属性, 避免全文搜串位 */
    {
        const char *data_field = json_find_key(json, "data");

        if (data_field != NULL)
            json = data_field;
    }

    for (size_t i = 0; i < PROP_NUM; i++) {
        double desired_val;

        if (!s_props[i].writable)
            continue;
        if (json_get_nested_number(json, s_props[i].key, &desired_val) == 0) {
            printf("desired %s = %g\n", s_props[i].key, desired_val);
            prop_apply(&s_props[i], desired_val);
            applied++;
        }
    }
    if (applied == 0)
        printf("desired reply has no writable props\n");
    return applied > 0 ? 0 : -1;
}

/*******************************************************************
** 函数名	: onenet_publish
** 函数描述	: 打包PUBLISH并入发送队列(链路1, 失败重发一次)
** 参数		: [in] topic: 主题
**          : [in] payload: 消息内容
**          : [in] plen: 消息长度
** 返回		: 0入队成功, -1失败
********************************************************************/
static int onenet_publish(const char *topic, const char *payload, int plen)
{
    uint8_t buf[ESP_PKTQ_SIZE];
    int pkt_len;

    /* 先把可读内容打出来, 十六进制转储交给发送队列 */
    printf("mqtt tx: %s\n", topic);
    if (plen > 0)
        printf("  payload: %.*s\n", plen, payload);

    pkt_len = mqtt_build_publish(buf, sizeof(buf), topic,
                                 (const uint8_t *)payload, plen);
    if (pkt_len <= 0) {
        printf("publish pack failed, topic %s\n", topic);
        return -1;
    }
    return esp_pktq_enqueue(ONENET_LINK_MQTT, buf, pkt_len, 1, NULL, NULL);
}

/*******************************************************************
** 函数名	: onenet_message
** 函数描述	: 平台下行PUBLISH按topic分派处理
** 参数		: [in] pub: PUBLISH字段
** 返回		: 无
********************************************************************/
static void onenet_message(const mqtt_pub_t *pub)
{
    char msg[600];
    int len = pub->payload_len < (int)sizeof(msg) - 1
            ? pub->payload_len : (int)sizeof(msg) - 1;

    memcpy(msg, pub->payload, len);
    msg[len] = '\0';
    printf("platform msg on %s\n  payload: %s\n", pub->topic, msg);

    if (strcmp(pub->topic, s_topic_post_reply) == 0 ||
        strcmp(pub->topic, s_topic_event_reply) == 0) {
        int code = -1;
        char errmsg[64];

        if (parse_reply_json(msg, &code, errmsg, sizeof(errmsg)) == 0)
            printf("report %s, errcode=%d errmsg=%s\n",
                   code == 0 || code == 200 ? "ok" : "failed", code, errmsg);
        else
            printf("reply unparsed: %s\n", msg);
    } else if (strcmp(pub->topic, s_topic_set) == 0) {
        char reply[160];
        char reply_topic[ONENET_TOPIC_MAX_LEN];
        int reply_len = build_set_reply(reply, sizeof(reply),
                                        parse_set_json(msg) == 0 ? 200 : 400);

        if (reply_len > 0) {
            snprintf(reply_topic, sizeof(reply_topic), ONENET_TOPIC_SET_REPLY,
                     ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);
            onenet_publish(reply_topic, reply, reply_len);
        }
    } else if (strcmp(pub->topic, s_topic_desired_reply) == 0) {
        parse_desired_reply(msg);
    } else {
        printf("unmatched topic, %d bytes ignored\n", pub->payload_len);
    }
}

/*******************************************************************
** 函数名	: onenet_frame
** 函数描述	: MQTT下行帧分派
** 参数		: [in] f: 帧描述
** 返回		: 无
********************************************************************/
static void onenet_frame(const mqtt_frame_t *f)
{
    switch (f->type & 0xF0) {
    case 0x20:                  /* CONNACK */
        if (f->remlen >= 2 && f->body[1] == 0) {
            s_connack_ok = 1;
            printf("mqtt connected (CONNACK)\n");
        } else {
            printf("connack refused, code=%d\n",
                   f->remlen >= 2 ? f->body[1] : -1);
        }
        break;
    case 0x90:                  /* SUBACK */
        s_suback_cnt++;
        printf("suback %d\n", s_suback_cnt);
        break;
    case 0xD0:                  /* PINGRESP */
        s_ping_pending = 0;
        break;
    case 0x30: {                /* PUBLISH下行 */
        mqtt_pub_t pub;

        if (mqtt_parse_publish(f, &pub) == 0)
            onenet_message(&pub);
        break;
    }
    default:
        break;
    }
}

/*******************************************************************
** 函数名	: onenet_link_feed
** 函数描述	: 链路1载荷入口: 入重组缓冲并切帧分发
** 参数		: [in] link: 链路号
**          : [in] data: 载荷数据
**          : [in] len: 载荷长度
** 返回		: 无
********************************************************************/
void onenet_link_feed(int link, const uint8_t *data, int len)
{
    mqtt_frame_t f;

    (void)link;
    mqtt_reasm_feed(data, len);
    while (mqtt_reasm_next(&f) == 0) {
        printf("mqtt rx: %s, %d bytes\n", mqtt_type_name(f.type), f.remlen);
        onenet_frame(&f);
        mqtt_reasm_consume();
    }
}

/*******************************************************************
** 函数名	: pump_until_flag
** 函数描述	: 循环pump直到标志被接收路径置位
** 参数		: [in] flag: 标志变量
**          : [in] timeout_ms: 超时毫秒数
** 返回		: 1置位, 0超时, -1串口异常
********************************************************************/
static int pump_until_flag(const int *flag, int timeout_ms)
{
    uint64_t deadline = esp_at_now_ms() + timeout_ms;

    while (!*flag) {
        if (esp_at_now_ms() >= deadline)
            return 0;
        if (esp_at_pump(100) < 0)
            return -1;
        esp_pktq_pump();
    }
    return 1;
}

/*******************************************************************
** 函数名	: delay_pump
** 函数描述	: 带收发的延时, 等待期间持续读串口并推发送队列, 防缓冲溢出
** 参数		: [in] ms: 延时毫秒数
** 返回		: 0正常, -1串口异常
********************************************************************/
static int delay_pump(int ms)
{
    uint64_t deadline = esp_at_now_ms() + ms;

    printf("wait %dms\n", ms);
    while (esp_at_now_ms() < deadline) {
        if (esp_at_pump(100) < 0)
            return -1;
        esp_pktq_pump();
    }
    return 0;
}

/*******************************************************************
** 函数名	: onenet_init
** 函数描述	: 初始化属性表与led模拟源
** 参数		: 无
** 返回		: 无
********************************************************************/
void onenet_init(void)
{
    g_msg_id = 1;
    g_led1 = g_led2 = 0;
    g_led_dirty = 0;

    snprintf(s_topic_post_reply, sizeof(s_topic_post_reply),
             ONENET_TOPIC_POST_REPLY, ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);
    snprintf(s_topic_event_reply, sizeof(s_topic_event_reply),
             ONENET_TOPIC_EVENT_REPLY, ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);
    snprintf(s_topic_set, sizeof(s_topic_set),
             ONENET_TOPIC_SET, ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);
    snprintf(s_topic_desired_reply, sizeof(s_topic_desired_reply),
             ONENET_TOPIC_DESIRED_REPLY, ONENET_PRODUCT_ID,
             ONENET_DEVICE_NAME);
}

/*******************************************************************
** 函数名	: onenet_set_led
** 函数描述	: 设置led事件值, 变化时置脏等主循环上报
** 参数		: [in] led1: led1值
**          : [in] led2: led2值
** 返回		: 无
********************************************************************/
static void onenet_set_led(int led1, int led2)
{
    if (led1 != g_led1 || led2 != g_led2) {
        g_led1 = led1;
        g_led2 = led2;
        g_led_dirty = 1;
        printf("led changed: led1=%d led2=%d\n", led1, led2);
    }
}

/*******************************************************************
** 函数名	: onenet_connect
** 函数描述	: 链路1建链: CIPSTART->CONNECT->CONNACK->订阅4个topic
** 参数		: [in] et_fallback_ok: 未校时是否允许用兜底过期时间建链
** 返回		: 0成功, -1失败
********************************************************************/
int onenet_connect(int et_fallback_ok)
{
    uint8_t buf[512];
    char token[ONENET_TOKEN_MAX_LEN];
    uint32_t expire_ts;
    int pkt_len, conn_ret;

    s_online = 0;
    s_sub_state = SUB_RUNNING;
    s_suback_cnt = 0;
    s_connack_ok = 0;
    s_ping_pending = 0;

    /* et优先取NTP校时时间戳加有效期; 补校时耗尽且允许兜底才用兜底值 */
    if (ntp_time_valid()) {
        expire_ts = (uint32_t)ntp_now_unix() + ONENET_TOKEN_VALID_SEC;
    } else if (et_fallback_ok) {
        expire_ts = ONENET_TOKEN_ET_FALLBACK;
    } else {
        printf("ntp time not synced, token et unavailable\n");
        goto fail;
    }
    if (onenet_token_build(expire_ts, token, sizeof(token)) != 0) {
        printf("token build failed\n");
        goto fail;
    }

    conn_ret = esp_at_cmd_exec(WIFI_CMD_CIPSTART_TCP, NULL,
                               ONENET_BROKER_HOST, ONENET_BROKER_PORT);
    if (conn_ret != ESP_AT_OK) {
        printf("tcp connect to %s:%d failed (%d)\n",
               ONENET_BROKER_HOST, ONENET_BROKER_PORT, conn_ret);
        goto fail;
    }

    pkt_len = mqtt_build_connect(buf, sizeof(buf), ONENET_CLIENT_ID,
                                 ONENET_USERNAME, token,
                                 ONENET_KEEPALIVE_SEC);
    if (pkt_len <= 0 ||
        esp_pktq_enqueue(ONENET_LINK_MQTT, buf, pkt_len, 1, NULL, NULL) != 0)
        goto fail;

    if (pump_until_flag(&s_connack_ok, ONENET_CONNACK_TIMEOUT_MS) != 1) {
        printf("connack timeout\n");
        goto fail;
    }

    /* 连上后缓一下再订阅, 平台侧会话建立需要时间 */
    if (delay_pump(ONENET_SUB_DELAY_MS) != 0)
        goto fail;

    /* 4个topic逐个订阅, 各等一个SUBACK */
    for (size_t i = 0; i < SUB_TOPIC_NUM; i++) {
        char topic[ONENET_TOPIC_MAX_LEN];
        uint64_t deadline;
        int expect = (int)i + 1;

        snprintf(topic, sizeof(topic), s_sub_topics[i],
                 ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);
        pkt_len = mqtt_build_subscribe(buf, sizeof(buf), topic, 0);
        if (pkt_len <= 0 ||
            esp_pktq_enqueue(ONENET_LINK_MQTT, buf, pkt_len, 1, NULL, NULL) != 0)
            goto fail;

        deadline = esp_at_now_ms() + ONENET_SUBACK_TIMEOUT_MS;
        while (s_suback_cnt < expect) {
            if (esp_at_now_ms() >= deadline) {
                printf("suback timeout: %s\n", topic);
                goto fail;
            }
            if (esp_at_pump(100) < 0)
                goto fail;
            esp_pktq_pump();
        }
    }
    s_sub_state = SUB_DONE;

    /* 订阅齐了缓一下再上报, 首次上报推迟到delay之后 */
    if (delay_pump(ONENET_FIRST_REPORT_DELAY_MS) != 0)
        goto fail;

    /* 先拉一次期望值 */
    {
        char body[256];
        char topic[ONENET_TOPIC_MAX_LEN];

        pkt_len = build_desired_get(body, sizeof(body));
        snprintf(topic, sizeof(topic), ONENET_TOPIC_DESIRED_GET,
                 ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);
        if (pkt_len > 0)
            onenet_publish(topic, body, pkt_len);
    }

    s_online = 1;
    /* 5秒延时已在上面delay_pump里耗掉, 上线即首次上报 */
    s_last_report = 0;
    s_last_ping = esp_at_now_ms();
    s_last_led_sim = esp_at_now_ms();
    printf("onenet online, keepalive %ds\n", ONENET_KEEPALIVE_SEC);
    return 0;

fail:
    s_sub_state = SUB_IDLE;
    esp_at_cmd_exec(WIFI_CMD_CIPCLOSE, NULL, ONENET_LINK_MQTT);
    return -1;
}

/*******************************************************************
** 函数名	: onenet_is_online
** 函数描述	: 查询链路1是否完成建链
** 参数		: 无
** 返回		: 1在线, 0离线
********************************************************************/
int onenet_is_online(void)
{
    return s_online;
}

/*******************************************************************
** 函数名	: onenet_process
** 函数描述	: 主循环周期调用, 驱动上报/心跳/led模拟源
** 参数		: 无
** 返回		: 无
********************************************************************/
void onenet_process(void)
{
    uint64_t now = esp_at_now_ms();
    char topic[ONENET_TOPIC_MAX_LEN];

    if (!s_online)
        return;

    /* 定时属性上报 */
    if (s_last_report == 0 ||
        now - s_last_report >= (uint64_t)ONENET_REPORT_SEC * 1000) {
        char json[ESP_PKTQ_SIZE];
        int len = build_property_post(json, sizeof(json));

        snprintf(topic, sizeof(topic), ONENET_TOPIC_POST,
                 ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);
        if (len > 0) {
            printf("property post (%d bytes)\n", len);
            onenet_publish(topic, json, len);
        }
        s_last_report = now;
    }

    /* led模拟源: 周期翻转, 演示event/post */
    if (now - s_last_led_sim >= (uint64_t)ONENET_LED_SIM_SEC * 1000) {
        onenet_set_led(!g_led1, !g_led2);
        s_last_led_sim = now;
    }
    if (g_led_dirty) {
        char json[256];
        int len = build_event_led(json, sizeof(json));

        snprintf(topic, sizeof(topic), ONENET_TOPIC_EVENT_POST,
                 ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);
        if (len > 0) {
            printf("led event post (%d bytes)\n", len);
            onenet_publish(topic, json, len);
        }
        g_led_dirty = 0;
    }

    /* 心跳: 50秒一PING, 10秒没回就判死 */
    if (now - s_last_ping >= (uint64_t)ONENET_HEARTBEAT_SEC * 1000) {
        uint8_t ping[2];

        mqtt_build_pingreq(ping);
        esp_pktq_enqueue(ONENET_LINK_MQTT, ping, 2, 0, NULL, NULL);
        s_last_ping = now;
        s_ping_pending = 1;
    } else if (s_ping_pending &&
               now - s_last_ping > ONENET_PINGRESP_TIMEOUT_MS) {
        printf("no pingresp in %dms, link lost\n", ONENET_PINGRESP_TIMEOUT_MS);
        onenet_on_link_closed(ONENET_LINK_MQTT);
    }
}

/*******************************************************************
** 函数名	: onenet_on_link_closed
** 函数描述	: 链路被模块关闭时的通知
** 参数		: [in] link: 链路号
** 返回		: 无
********************************************************************/
void onenet_on_link_closed(int link)
{
    if (link != ONENET_LINK_MQTT)
        return;
    if (s_online || s_sub_state == SUB_RUNNING)
        printf("link 1 closed\n");
    s_online = 0;
    s_sub_state = SUB_IDLE;
    s_suback_cnt = 0;
    s_ping_pending = 0;
}

/*******************************************************************
** 函数名	: onenet_on_wifi_lost
** 函数描述	: WIFI DISCONNECT通知, 云端状态一并作废
** 参数		: 无
** 返回		: 无
********************************************************************/
void onenet_on_wifi_lost(void)
{
    printf("wifi disconnect\n");
    s_online = 0;
    s_sub_state = SUB_IDLE;
    s_suback_cnt = 0;
    s_ping_pending = 0;
}
