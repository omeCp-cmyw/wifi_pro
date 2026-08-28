#include <stdio.h>
#include <string.h>

#include "esp_pktq.h"
#include "esp_at.h"

/*
 * 发送侧用静态内存池管理在途包。串口是串行的, 同一时刻只有
 * 一个包在等SEND OK, 其余节点排队。
 */

enum {
    NODE_FREE,
    NODE_PEND,          /* 排队中, 等pump发CIPSEND */
    NODE_WAIT_ACK       /* 裸字节已写, 等SEND OK/SEND FAIL */
};

#define PKTQ_ACK_TIMEOUT 5000   /* SEND OK等待时间, 毫秒 */

typedef struct {
    int state;
    int link;
    int len;
    int retry;
    uint64_t deadline;
    uint8_t data[ESP_PKTQ_SIZE];
    esp_pktq_done_cb_t cb;
    void *arg;
} pktq_node_t;

static pktq_node_t s_nodes[ESP_PKTQ_NUM];
static int s_ack_idx = -1;      /* 正在等SEND OK的节点下标 */

/*******************************************************************
** 函数名	: node_finish
** 函数描述	: 节点回池并通知发送结果
** 参数		: [in] idx: 节点下标
**          : [in] ok: 1成功, 0失败
** 返回		: 无
********************************************************************/
static void node_finish(int idx, int ok)
{
    pktq_node_t *node = &s_nodes[idx];
    esp_pktq_done_cb_t cb = node->cb;
    void *arg = node->arg;
    int link = node->link;

    node->state = NODE_FREE;
    node->cb = NULL;
    if (s_ack_idx == idx)
        s_ack_idx = -1;
    if (cb)
        cb(link, ok, arg);
}

/*******************************************************************
** 函数名	: pktq_send_result
** 函数描述	: SEND OK/SEND FAIL异步通知入口
** 参数		: [in] link: 链路号(SEND OK不带链路号, 未使用)
**          : [in] ok: 1成功, 0失败
** 返回		: 无
********************************************************************/
static void pktq_send_result(int link, int ok)
{
    pktq_node_t *node;
    int idx = s_ack_idx;

    (void)link;
    if (idx < 0)
        return;
    node = &s_nodes[idx];

    if (ok) {
        node_finish(idx, 1);
        return;
    }
    if (node->retry-- > 0) {
        printf("send fail on link %d, retry left %d\n", node->link, node->retry);
        node->state = NODE_PEND;
        s_ack_idx = -1;
    } else {
        node_finish(idx, 0);
    }
}

/*******************************************************************
** 函数名	: esp_pktq_init
** 函数描述	: 初始化静态包池并挂接SEND OK/SEND FAIL通知
** 参数		: 无
** 返回		: 无
********************************************************************/
void esp_pktq_init(void)
{
    memset(s_nodes, 0, sizeof(s_nodes));
    s_ack_idx = -1;
    esp_at_on_send_result(pktq_send_result);
}

/*******************************************************************
** 函数名	: esp_pktq_enqueue
** 函数描述	: 拷贝数据入队, 由pump负责发CIPSEND与裸字节
** 参数		: [in] link: 链路号
**          : [in] data: 待发数据
**          : [in] len: 数据长度
**          : [in] retry: 失败重发次数(不含首次)
**          : [in] cb: 结果回调(可为NULL)
**          : [in] arg: 回调参数
** 返回		: 0成功, -1池满或参数非法
********************************************************************/
int esp_pktq_enqueue(int link, const uint8_t *data, int len, int retry,
                     esp_pktq_done_cb_t cb, void *arg)
{
    pktq_node_t *node = NULL;

    if (data == NULL || len <= 0 || len > ESP_PKTQ_SIZE || retry < 0)
        return -1;

    for (int idx = 0; idx < ESP_PKTQ_NUM; idx++) {
        if (s_nodes[idx].state == NODE_FREE) {
            node = &s_nodes[idx];
            break;
        }
    }
    if (node == NULL) {
        printf("pktq full, %d bytes dropped\n", len);
        return -1;
    }

    node->link = link;
    node->len = len;
    node->retry = retry;
    memcpy(node->data, data, len);
    node->cb = cb;
    node->arg = arg;
    node->state = NODE_PEND;
    printf("pktq: link %d queued %d bytes\n", link, len);
    return 0;
}

/*******************************************************************
** 函数名	: esp_pktq_pump
** 函数描述	: 主循环周期调用, 推进各节点的发送状态机
** 参数		: 无
** 返回		: 无
********************************************************************/
void esp_pktq_pump(void)
{
    pktq_node_t *node;

    /* 在途包的SEND OK超时检查 */
    if (s_ack_idx >= 0) {
        node = &s_nodes[s_ack_idx];
        if (esp_at_now_ms() > node->deadline) {
            int idx = s_ack_idx;

            printf("send ack timeout on link %d\n", node->link);
            if (node->retry-- > 0) {
                node->state = NODE_PEND;
                s_ack_idx = -1;
            } else {
                node_finish(idx, 0);
            }
        }
    }
    /* 一次只发一个, 等上一个收敛 */
    if (s_ack_idx >= 0)
        return;

    for (int idx = 0; idx < ESP_PKTQ_NUM; idx++) {
        int send_ret;

        node = &s_nodes[idx];
        if (node->state != NODE_PEND)
            continue;

        send_ret = esp_at_cmd_exec(WIFI_CMD_CIPSEND, NULL,
                                   node->link, node->len);
        if (send_ret == ESP_AT_OK) {
            esp_at_hex_dump("[TX] pktq payload", node->data, node->len);
            if (esp_at_write_raw(node->data, node->len) != 0) {
                node_finish(idx, 0);
                break;
            }
            node->state = NODE_WAIT_ACK;
            node->deadline = esp_at_now_ms() + PKTQ_ACK_TIMEOUT;
            s_ack_idx = idx;
        } else if (send_ret == ESP_AT_FATAL || node->retry-- <= 0) {
            node_finish(idx, 0);
        } else {
            printf("cipsend link %d failed, retry left %d\n",
                   node->link, node->retry);
        }
        break;
    }
}

/*******************************************************************
** 函数名	: esp_pktq_busy
** 函数描述	: 查询是否还有节点在途
** 参数		: 无
** 返回		: 1有在途包, 0空闲
********************************************************************/
int esp_pktq_busy(void)
{
    for (int i = 0; i < ESP_PKTQ_NUM; i++) {
        if (s_nodes[i].state != NODE_FREE)
            return 1;
    }
    return 0;
}
