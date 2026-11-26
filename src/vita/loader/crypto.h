/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VITA_CRYPTO_H
#define SNX_VITA_CRYPTO_H
#include <openssl/evp.h>
#include <stdbool.h>
#include <stddef.h>

/* OpenSSL's contexts the import's ciphers and HMACs reuse, one for each sector of a file */
typedef struct {
    EVP_CIPHER_CTX* cipher;
    EVP_MAC* mac;
    EVP_MAC_CTX* hmac;
} VitaCrypto;

bool vita_crypto_open(VitaCrypto* c);
void vita_crypto_close(VitaCrypto* c);
/* size bytes of in through cipher (EVP_aes_128_cbc() and the others, no padding, so size a multiple of its block but
   for CTR) into out, which may be in */
bool vita_crypt(VitaCrypto* c, const EVP_CIPHER* cipher, bool encrypt, const unsigned char* key,
                const unsigned char* iv, const unsigned char* in, unsigned char* out, size_t size);
/* HMAC-SHA1 of size bytes at data under key */
bool vita_hmac_sha1(VitaCrypto* c, const unsigned char* key, size_t key_size, const void* data, size_t size,
                    unsigned char out[20]);
#endif
