/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "vita/loader/crypto.h"
#include <openssl/core_names.h>
#include <openssl/params.h>

bool vita_crypto_open(VitaCrypto* c) {
    *c = (VitaCrypto){0};
    c->cipher = EVP_CIPHER_CTX_new();
    c->mac = EVP_MAC_fetch(NULL, OSSL_MAC_NAME_HMAC, NULL);
    c->hmac = c->mac ? EVP_MAC_CTX_new(c->mac) : NULL;
    if (c->cipher && c->hmac) return true;
    vita_crypto_close(c);
    return false;
}
void vita_crypto_close(VitaCrypto* c) {
    EVP_MAC_CTX_free(c->hmac);
    EVP_MAC_free(c->mac);
    EVP_CIPHER_CTX_free(c->cipher);
    *c = (VitaCrypto){0};
}
bool vita_crypt(VitaCrypto* c, const EVP_CIPHER* cipher, bool encrypt, const unsigned char* key,
                const unsigned char* iv, const unsigned char* in, unsigned char* out, size_t size) {
    int done = 0, last = 0;
    if (!size) return true;
    if (size > 0x7fffffffu || EVP_CipherInit_ex(c->cipher, cipher, NULL, key, iv, encrypt) != 1 ||
        EVP_CIPHER_CTX_set_padding(c->cipher, 0) != 1 || EVP_CipherUpdate(c->cipher, out, &done, in, (int)size) != 1 ||
        EVP_CipherFinal_ex(c->cipher, out + done, &last) != 1)
        return false;
    return (size_t)done + (size_t)last == size;
}
bool vita_hmac_sha1(VitaCrypto* c, const unsigned char* key, size_t key_size, const void* data, size_t size,
                    unsigned char out[20]) {
    char digest[] = "SHA1";
    OSSL_PARAM params[] = {OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digest, 0),
                           OSSL_PARAM_construct_end()};
    size_t length = 0;
    return EVP_MAC_init(c->hmac, key, key_size, params) == 1 && EVP_MAC_update(c->hmac, data, size) == 1 &&
           EVP_MAC_final(c->hmac, out, &length, 20) == 1 && length == 20;
}
