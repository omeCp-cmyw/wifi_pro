#ifndef ONENET_CONFIG_H
#define ONENET_CONFIG_H

/* 串口与WiFi */
#define ONENET_SERIAL_DEV       "/dev/ttyUSB0"
#define ONENET_WIFI_SSID        "123"
#define ONENET_WIFI_PASS        "yw22334455"

/* 链路0: NTP校时 */
#define ONENET_NTP_SERVER       "ntp.aliyun.com"
#define ONENET_NTP_PORT         123
#define ONENET_NTP_TIMEOUT_MS   10000   /* 单次应答等待 */
#define ONENET_NTP_TRIES        3       /* 请求重发次数 */
#define ONENET_NTP_RESYNC_SEC   3600    /* 主循环重校时周期 */
#define ONENET_NTP_RECONN_TRIES 3       /* 重连流程内补校时次数上限, 耗尽用兜底时间 */

/* 链路1: OneNET broker */
#define ONENET_BROKER_HOST      "mqtts.heclouds.com"
#define ONENET_BROKER_PORT      1883
#define ONENET_KEEPALIVE_SEC    60
#define ONENET_CLIENT_ID        "smartdap"
#define ONENET_USERNAME         "X9Dcio5cI0"

/* 设备鉴权token参数, 登录密码由onenet_token_build运行时计算 */
#define ONENET_ACCESS_KEY       "THNpOEhkcFVXb1VBSFE5b0JnWmZDbFBBcmJYUUtyQkM="
#define ONENET_TOKEN_VERSION    "2018-10-31"
#define ONENET_TOKEN_METHOD     "md5"
#define ONENET_TOKEN_VALID_SEC  2592000     /* token有效期30天, et=当前时间+有效期 */
#define ONENET_TOKEN_ET_FALLBACK 1872999428 /* 补校时耗尽后的兜底过期时间(2029年) */

#define ONENET_PRODUCT_ID       "X9Dcio5cI0"
#define ONENET_DEVICE_NAME      "smartdap"

/* topic模板, 与onenet_mqtt工程保持一致 */
#define ONENET_TOPIC_POST          "$sys/%s/%s/thing/property/post"
#define ONENET_TOPIC_POST_REPLY    "$sys/%s/%s/thing/property/post/reply"
#define ONENET_TOPIC_EVENT_POST    "$sys/%s/%s/thing/event/post"
#define ONENET_TOPIC_EVENT_REPLY   "$sys/%s/%s/thing/event/post/reply"
#define ONENET_TOPIC_SET           "$sys/%s/%s/thing/property/set"
#define ONENET_TOPIC_SET_REPLY     "$sys/%s/%s/thing/property/set_reply"
#define ONENET_TOPIC_DESIRED_GET   "$sys/%s/%s/thing/property/desired/get"
#define ONENET_TOPIC_DESIRED_REPLY "$sys/%s/%s/thing/property/desired/get/reply"
#define ONENET_TOPIC_MAX_LEN       128

/* 建链握手超时(毫秒) */
#define ONENET_CONNACK_TIMEOUT_MS 10000
#define ONENET_SUBACK_TIMEOUT_MS  10000
#define ONENET_PINGRESP_TIMEOUT_MS 10000

/* 建链节奏: CONNACK后缓一下再订阅, 订阅齐了缓一下再上报 */
#define ONENET_SUB_DELAY_MS     2000
#define ONENET_FIRST_REPORT_DELAY_MS 5000

/* 主循环节奏(秒) */
#define ONENET_REPORT_SEC       10      /* 属性上报周期 */
#define ONENET_HEARTBEAT_SEC    50      /* PINGREQ周期, 小于keepalive */
#define ONENET_LED_SIM_SEC      30      /* led模拟源翻转周期 */
#define ONENET_AP_CHECK_SEC     5       /* AT+CWJAP?关联检测周期 */
#define ONENET_RECONN_MIN_SEC   5       /* 重连退避起点 */
#define ONENET_RECONN_MAX_SEC   60      /* 重连退避上限 */

/* 链路号分配 */
#define ONENET_LINK_NTP         0
#define ONENET_LINK_MQTT        1

#endif
