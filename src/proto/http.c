#include <stdio.h>
#include <string.h>

#include "http.h"

/*******************************************************************
** 函数名	: http_build_request
** 函数描述	: 拼装完整HTTP POST请求(请求行+头+体), Connection: close
**          : 让服务端主动断开, 便于AT侧判定应答结束
** 参数		: [out] out: 输出缓冲
**          : [in] out_size: 缓冲大小
**          : [in] host: Host头取值
**          : [in] path_query: 请求路径含查询串
**          : [in] token: 设备鉴权token头
**          : [in] body: 请求体
**          : [in] body_len: 请求体长度
** 返回		: 请求总长度, -1缓冲不够
********************************************************************/
int http_build_request(char *out, int out_size, const char *host,
                       const char *path_query, const char *token,
                       const char *body, int body_len)
{
    int head_len;

    if (body_len < 0 || body_len >= out_size)
        return -1;

    head_len = snprintf(out, (size_t)out_size,
                        "POST %s HTTP/1.1\r\n"
                        "Host: %s\r\n"
                        "Content-Type: application/json\r\n"
                        "Content-Length: %d\r\n"
                        "token: %s\r\n"
                        "Connection: close\r\n"
                        "\r\n",
                        path_query, host, body_len, token);
    if (head_len < 0 || head_len + body_len >= out_size)
        return -1;

    memcpy(out + head_len, body, (size_t)body_len);
    out[head_len + body_len] = '\0';
    return head_len + body_len;
}

/*******************************************************************
** 函数名	: http_resp_status
** 函数描述	: 判定OneNET应答: 体含"errno":0为成功, 体含1104或
**          : "token expired"为token过期, 其余按失败
** 参数		: [in] resp: 已收到的应答内容(需以'\0'结尾)
**          : [in] len: 应答长度
** 返回		: HTTP_RESP_OK / HTTP_RESP_TOKEN_EXP / HTTP_RESP_FAIL
********************************************************************/
int http_resp_status(const char *resp, int len)
{
    if (resp == NULL || len <= 0)
        return HTTP_RESP_FAIL;

    /* 平台token过期时回复1104错误码, 文本描述两种形态都出现过 */
    if (strstr(resp, "1104") != NULL ||
        strstr(resp, "token expired") != NULL)
        return HTTP_RESP_TOKEN_EXP;

    /* 成功回执体里errno为0, 冒号后可能带空格 */
    if (strstr(resp, "\"errno\":0") != NULL ||
        strstr(resp, "\"errno\": 0") != NULL)
        return HTTP_RESP_OK;

    return HTTP_RESP_FAIL;
}
