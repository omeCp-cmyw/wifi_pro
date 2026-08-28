#ifndef HTTP_H
#define HTTP_H

/* http_resp_status 的判定结果 */
#define HTTP_RESP_OK            0
#define HTTP_RESP_TOKEN_EXP     1
#define HTTP_RESP_FAIL         -1

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
                       const char *body, int body_len);

/*******************************************************************
** 函数名	: http_resp_status
** 函数描述	: 判定OneNET应答: 体含"errno":0为成功, 体含1104或
**          : "token expired"为token过期, 其余按失败
** 参数		: [in] resp: 已收到的应答内容
**          : [in] len: 应答长度
** 返回		: HTTP_RESP_OK / HTTP_RESP_TOKEN_EXP / HTTP_RESP_FAIL
********************************************************************/
int http_resp_status(const char *resp, int len);

#endif
