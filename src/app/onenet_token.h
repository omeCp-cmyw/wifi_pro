#ifndef ONENET_TOKEN_H
#define ONENET_TOKEN_H

#include <stdint.h>

/* 生成的token串需要的最小缓冲(含结尾'\0') */
#define ONENET_TOKEN_MAX_LEN    200

/*******************************************************************
** 函数名	: onenet_token_build
** 函数描述	: 计算OneNET设备鉴权token: 解码访问密钥, 对签名串做
**             HMAC-MD5, sign经base64与URL编码后与各参数拼成完整串,
**             凭证参数化, MQTT与HTTP两套设备可共用
** 参数		: [in] product_id: 产品ID
**          : [in] device_name: 设备名, 与product_id拼成res
**          : [in] access_key: 访问密钥(base64文本)
**          : [in] expire_ts: 访问过期时间(unix秒)
**          : [out] token: token输出缓冲
**          : [in] token_size: 缓冲大小(不小于ONENET_TOKEN_MAX_LEN)
** 返回		: 0成功, -1失败
********************************************************************/
int onenet_token_build(const char *product_id, const char *device_name,
                       const char *access_key, uint32_t expire_ts,
                       char *token, int token_size);

#endif
