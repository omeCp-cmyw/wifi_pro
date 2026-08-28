#include <stdio.h>
#include <string.h>

#include "onenet_token.h"
#include "onenet_config.h"
#include "../crypto/md5.h"

static const char BASE64_TABLE[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* OneNET只规定这几个字符需要编码, 其余原样保留不走全量转义 */
static const char URL_SPECIAL[] = "+ /?%#&=";

/*******************************************************************
** 函数名	: base64_char_value
** 函数描述	: base64字符转6位数值
** 参数		: [in] ch: base64字符
** 返回		: 0~63合法, -1非法字符
********************************************************************/
static int base64_char_value(char ch)
{
    const char *pos = strchr(BASE64_TABLE, ch);

    if (pos == NULL)
        return -1;
    return (int)(pos - BASE64_TABLE);
}

/*******************************************************************
** 函数名	: base64_decode
** 函数描述	: base64串解码为二进制, 忽略结尾填充'='
** 参数		: [in] in: base64串
**          : [out] out: 输出缓冲
**          : [in] out_max: 缓冲大小
** 返回		: 解码字节数, -1非法或缓冲不够
********************************************************************/
static int base64_decode(const char *in, uint8_t *out, int out_max)
{
    int out_len = 0;
    int bits_used = 0;
    uint32_t accum = 0;

    while (*in != '\0') {
        int value;

        if (*in == '=') {
            in++;
            continue;
        }
        value = base64_char_value(*in);
        if (value < 0)
            return -1;
        accum = (accum << 6) | (uint32_t)value;
        bits_used += 6;
        if (bits_used >= 8) {
            if (out_len >= out_max)
                return -1;
            bits_used -= 8;
            out[out_len++] = (uint8_t)((accum >> bits_used) & 0xff);
        }
        in++;
    }
    return out_len;
}

/*******************************************************************
** 函数名	: base64_encode
** 函数描述	: 二进制编码为base64串, 不足3字节用'='填充
** 参数		: [in] in: 二进制数据
**          : [in] in_len: 数据长度
**          : [out] out: 输出缓冲(含结尾'\0')
**          : [in] out_max: 缓冲大小
** 返回		: 0成功, -1缓冲不够
********************************************************************/
static int base64_encode(const uint8_t *in, int in_len, char *out, int out_max)
{
    int needed = (in_len + 2) / 3 * 4 + 1;
    int i, out_pos = 0;

    if (out_max < needed)
        return -1;
    for (i = 0; i + 2 < in_len; i += 3) {
        uint32_t group = ((uint32_t)in[i] << 16) |
                         ((uint32_t)in[i + 1] << 8) | in[i + 2];

        out[out_pos++] = BASE64_TABLE[(group >> 18) & 0x3f];
        out[out_pos++] = BASE64_TABLE[(group >> 12) & 0x3f];
        out[out_pos++] = BASE64_TABLE[(group >> 6) & 0x3f];
        out[out_pos++] = BASE64_TABLE[group & 0x3f];
    }
    if (in_len - i == 1) {
        uint32_t group = (uint32_t)in[i] << 16;

        out[out_pos++] = BASE64_TABLE[(group >> 18) & 0x3f];
        out[out_pos++] = BASE64_TABLE[(group >> 12) & 0x3f];
        out[out_pos++] = '=';
        out[out_pos++] = '=';
    } else if (in_len - i == 2) {
        uint32_t group = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);

        out[out_pos++] = BASE64_TABLE[(group >> 18) & 0x3f];
        out[out_pos++] = BASE64_TABLE[(group >> 12) & 0x3f];
        out[out_pos++] = BASE64_TABLE[(group >> 6) & 0x3f];
        out[out_pos++] = '=';
    }
    out[out_pos] = '\0';
    return 0;
}

/*******************************************************************
** 函数名	: url_encode_value
** 函数描述	: 按OneNET规定对token参数值做URL编码
** 参数		: [in] in: 原始值
**          : [out] out: 输出缓冲(含结尾'\0')
**          : [in] out_max: 缓冲大小
** 返回		: 0成功, -1缓冲不够
********************************************************************/
static int url_encode_value(const char *in, char *out, int out_max)
{
    int out_pos = 0;

    while (*in != '\0') {
        if (strchr(URL_SPECIAL, *in) != NULL) {
            if (out_pos + 4 > out_max)
                return -1;
            out_pos += snprintf(out + out_pos, (size_t)(out_max - out_pos),
                                "%%%02X", (unsigned char)*in);
        } else {
            if (out_pos + 2 > out_max)
                return -1;
            out[out_pos++] = *in;
        }
        in++;
    }
    if (out_pos + 1 > out_max)
        return -1;
    out[out_pos] = '\0';
    return 0;
}

/*******************************************************************
** 函数名	: hmac_md5
** 函数描述	: RFC2104 HMAC-MD5, 密钥超块长先散列
** 参数		: [in] key: 密钥
**          : [in] key_len: 密钥长度
**          : [in] msg: 待签名串
**          : [out] digest: 16字节签名输出
** 返回		: 无
********************************************************************/
static void hmac_md5(const uint8_t *key, int key_len, const char *msg,
                     uint8_t digest[MD5_DIGEST_LENGTH])
{
    uint8_t key_block[MD5_CBLOCK];
    uint8_t pad_block[MD5_CBLOCK];
    uint8_t inner_digest[MD5_DIGEST_LENGTH];
    MD5_CTX ctx;
    int i;

    memset(key_block, 0, sizeof(key_block));
    if (key_len > MD5_CBLOCK)
        MD5(key, (size_t)key_len, key_block);
    else
        memcpy(key_block, key, (size_t)key_len);

    /* 内层: md5((key^ipad) || msg) */
    for (i = 0; i < MD5_CBLOCK; i++)
        pad_block[i] = key_block[i] ^ 0x36;
    MD5_Init(&ctx);
    MD5_Update(&ctx, pad_block, MD5_CBLOCK);
    MD5_Update(&ctx, msg, strlen(msg));
    MD5_Final(inner_digest, &ctx);

    /* 外层: md5((key^opad) || inner) */
    for (i = 0; i < MD5_CBLOCK; i++)
        pad_block[i] = key_block[i] ^ 0x5c;
    MD5_Init(&ctx);
    MD5_Update(&ctx, pad_block, MD5_CBLOCK);
    MD5_Update(&ctx, inner_digest, MD5_DIGEST_LENGTH);
    MD5_Final(digest, &ctx);
}

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
                       char *token, int token_size)
{
    char res[96];
    char res_enc[128];
    char sign_str[160];
    char sign_b64[32];
    char sign_enc[40];
    uint8_t key_raw[64];
    uint8_t hmac_digest[MD5_DIGEST_LENGTH];
    int key_len, used;

    used = snprintf(res, sizeof(res), "products/%s/devices/%s",
                    product_id, device_name);
    if (used < 0 || used >= (int)sizeof(res))
        return -1;

    key_len = base64_decode(access_key, key_raw, sizeof(key_raw));
    if (key_len < 0)
        return -1;

    /* 签名串按et/method/res/version顺序用换行分隔, 末尾无换行 */
    used = snprintf(sign_str, sizeof(sign_str), "%u\n%s\n%s\n%s",
                    (unsigned)expire_ts, ONENET_TOKEN_METHOD, res,
                    ONENET_TOKEN_VERSION);
    if (used < 0 || used >= (int)sizeof(sign_str))
        return -1;

    hmac_md5(key_raw, key_len, sign_str, hmac_digest);
    if (base64_encode(hmac_digest, MD5_DIGEST_LENGTH, sign_b64,
                      sizeof(sign_b64)) != 0)
        return -1;
    if (url_encode_value(res, res_enc, sizeof(res_enc)) != 0 ||
        url_encode_value(sign_b64, sign_enc, sizeof(sign_enc)) != 0)
        return -1;

    used = snprintf(token, (size_t)token_size,
                    "version=%s&res=%s&et=%u&method=%s&sign=%s",
                    ONENET_TOKEN_VERSION, res_enc, (unsigned)expire_ts,
                    ONENET_TOKEN_METHOD, sign_enc);
    if (used < 0 || used >= token_size)
        return -1;
    return 0;
}
