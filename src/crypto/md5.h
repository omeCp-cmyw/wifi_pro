/*
 * Copyright 1995-2020 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#ifndef _MD5_H_
# define _MD5_H_
# pragma once

#  include <stddef.h>
#  include <stdbool.h>

#  ifdef  __cplusplus
extern "C" {
#  endif

#  define MD5_DIGEST_LENGTH 16

/*
 * !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
 * ! MD5_LONG has to be at least 32 bits wide.                     !
 * !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
 */
#   define MD5_LONG unsigned int

#   define MD5_CBLOCK      64
#   define MD5_LBLOCK      (MD5_CBLOCK/4)

typedef struct MD5state_st {
    MD5_LONG A, B, C, D;
    MD5_LONG Nl, Nh;
    MD5_LONG data[MD5_LBLOCK];
    unsigned int num;
} MD5_CTX;

int MD5_Init(MD5_CTX *c);
int MD5_Update(MD5_CTX *c, const void *data, size_t len);
int MD5_Final(unsigned char *md, MD5_CTX *c);
unsigned char *MD5(const unsigned char *d, size_t n, unsigned char *md);
// void MD5_Transform(MD5_CTX *c, const unsigned char *b);

/*******************************************************************
 * 函数名称:	get_file_md5
 * 函数描述:	获取文件的MD5�?
 * 函数参数:	[in] pFileName:		文件�?(带路�?)
 * 函数参数:	[in] buf:			存放MD5值缓冲区
 * 函数参数:	[in] bufLen:		缓冲区大�?
 * 函数返回:	false:获取成功,	true:获取失败
********************************************************************/
bool get_file_md5(const char *pFileName, unsigned char *buf, unsigned short bufLen);

void md5_block_data_order(MD5_CTX *c, const void *p, size_t num);

#define DATA_ORDER_IS_LITTLE_ENDIAN

#define HASH_LONG               MD5_LONG
#define HASH_CTX                MD5_CTX
#define HASH_CBLOCK             MD5_CBLOCK
#define HASH_UPDATE             MD5_Update
#define HASH_TRANSFORM          MD5_Transform
#define HASH_FINAL              MD5_Final
#define HASH_MAKE_STRING(c,s)   do {    \
        unsigned long ll;               \
        ll=(c)->A; (void)HOST_l2c(ll,(s));      \
        ll=(c)->B; (void)HOST_l2c(ll,(s));      \
        ll=(c)->C; (void)HOST_l2c(ll,(s));      \
        ll=(c)->D; (void)HOST_l2c(ll,(s));      \
        } while (0)
#define HASH_BLOCK_DATA_ORDER   md5_block_data_order

#if !defined(DATA_ORDER_IS_BIG_ENDIAN) && !defined(DATA_ORDER_IS_LITTLE_ENDIAN)
# error "DATA_ORDER must be defined!"
#endif

#ifndef HASH_CBLOCK
# error "HASH_CBLOCK must be defined!"
#endif
#ifndef HASH_LONG
# error "HASH_LONG must be defined!"
#endif
#ifndef HASH_CTX
# error "HASH_CTX must be defined!"
#endif

#ifndef HASH_UPDATE
# error "HASH_UPDATE must be defined!"
#endif
#ifndef HASH_TRANSFORM
# error "HASH_TRANSFORM must be defined!"
#endif
#ifndef HASH_FINAL
# error "HASH_FINAL must be defined!"
#endif

#ifndef HASH_BLOCK_DATA_ORDER
# error "HASH_BLOCK_DATA_ORDER must be defined!"
#endif

#define ROTATE(a,n)     (((a)<<(n))|(((a)&0xffffffff)>>(32-(n))))

#ifndef PEDANTIC
# if defined(__GNUC__) && __GNUC__>=2 && \
     !defined(OPENSSL_NO_ASM) && !defined(OPENSSL_NO_INLINE_ASM)
#  if defined(__riscv_zbb) || defined(__riscv_zbkb)
#   if __riscv_xlen == 64
#   undef ROTATE
#   define ROTATE(x, n) ({ MD32_REG_T ret;            \
                       asm ("roriw %0, %1, %2"        \
                       : "=r"(ret)                    \
                       : "r"(x), "i"(32 - (n))); ret;})
#   endif
#   if __riscv_xlen == 32
#   undef ROTATE
#   define ROTATE(x, n) ({ MD32_REG_T ret;            \
                       asm ("rori %0, %1, %2"         \
                       : "=r"(ret)                    \
                       : "r"(x), "i"(32 - (n))); ret;})
#   endif
#  endif
# endif
#endif

#if defined(DATA_ORDER_IS_BIG_ENDIAN)

# define HOST_c2l(c,l)  (l =(((unsigned long)(*((c)++)))<<24),          \
                         l|=(((unsigned long)(*((c)++)))<<16),          \
                         l|=(((unsigned long)(*((c)++)))<< 8),          \
                         l|=(((unsigned long)(*((c)++)))    )           )
# define HOST_l2c(l,c)  (*((c)++)=(unsigned char)(((l)>>24)&0xff),      \
                         *((c)++)=(unsigned char)(((l)>>16)&0xff),      \
                         *((c)++)=(unsigned char)(((l)>> 8)&0xff),      \
                         *((c)++)=(unsigned char)(((l)    )&0xff),      \
                         l)

#elif defined(DATA_ORDER_IS_LITTLE_ENDIAN)

# define HOST_c2l(c,l)  (l =(((unsigned long)(*((c)++)))    ),          \
                         l|=(((unsigned long)(*((c)++)))<< 8),          \
                         l|=(((unsigned long)(*((c)++)))<<16),          \
                         l|=(((unsigned long)(*((c)++)))<<24)           )
# define HOST_l2c(l,c)  (*((c)++)=(unsigned char)(((l)    )&0xff),      \
                         *((c)++)=(unsigned char)(((l)>> 8)&0xff),      \
                         *((c)++)=(unsigned char)(((l)>>16)&0xff),      \
                         *((c)++)=(unsigned char)(((l)>>24)&0xff),      \
                         l)

#endif

#ifndef MD32_REG_T
# if defined(__alpha) || defined(__sparcv9) || defined(__mips)
#  define MD32_REG_T long
/*
 * This comment was originally written for MD5, which is why it
 * discusses A-D. But it basically applies to all 32-bit digests,
 * which is why it was moved to common header file.
 *
 * In case you wonder why A-D are declared as long and not
 * as MD5_LONG. Doing so results in slight performance
 * boost on LP64 architectures. The catch is we don't
 * really care if 32 MSBs of a 64-bit register get polluted
 * with eventual overflows as we *save* only 32 LSBs in
 * *either* case. Now declaring 'em long excuses the compiler
 * from keeping 32 MSBs zeroed resulting in 13% performance
 * improvement under SPARC Solaris7/64 and 5% under AlphaLinux.
 * Well, to be honest it should say that this *prevents*
 * performance degradation.
 */
# else
/*
 * Above is not absolute and there are LP64 compilers that
 * generate better code if MD32_REG_T is defined int. The above
 * pre-processor condition reflects the circumstances under which
 * the conclusion was made and is subject to further extension.
 */
#  define MD32_REG_T int
# endif
#endif

/*-
#define F(x,y,z)        (((x) & (y))  |  ((~(x)) & (z)))
#define G(x,y,z)        (((x) & (z))  |  ((y) & (~(z))))
*/

/*
 * As pointed out by Wei Dai, the above can be simplified to the code
 * below.  Wei attributes these optimizations to Peter Gutmann's
 * SHS code, and he attributes it to Rich Schroeppel.
 */
#define F(b,c,d)        ((((c) ^ (d)) & (b)) ^ (d))
#define G(b,c,d)        ((((b) ^ (c)) & (d)) ^ (c))
#define H(b,c,d)        ((b) ^ (c) ^ (d))
#define I(b,c,d)        (((~(d)) | (b)) ^ (c))

#define R0(a,b,c,d,k,s,t) { \
        a+=((k)+(t)+F((b),(c),(d))); \
        a=ROTATE(a,s); \
        a+=b; };

#define R1(a,b,c,d,k,s,t) { \
        a+=((k)+(t)+G((b),(c),(d))); \
        a=ROTATE(a,s); \
        a+=b; };

#define R2(a,b,c,d,k,s,t) { \
        a+=((k)+(t)+H((b),(c),(d))); \
        a=ROTATE(a,s); \
        a+=b; };

#define R3(a,b,c,d,k,s,t) { \
        a+=((k)+(t)+I((b),(c),(d))); \
        a=ROTATE(a,s); \
        a+=b; };


#  ifdef  __cplusplus
}
#  endif

#endif