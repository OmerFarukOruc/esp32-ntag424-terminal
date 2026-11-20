/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <openssl/evp.h>
#include <openssl/core_names.h>
#include <string.h>
#include <stdint.h>
#include "esp_err.h"
int host_cmac_fail;
static esp_err_t cbc(int enc, const uint8_t key[16], const uint8_t iv[16], const uint8_t *in, size_t len, uint8_t *out)
{
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new(); int n = 0, m = 0;
    int ok = EVP_CipherInit_ex(c, EVP_aes_128_cbc(), NULL, key, iv, enc) && EVP_CIPHER_CTX_set_padding(c, 0) &&
             EVP_CipherUpdate(c, out, &n, in, (int)len) && EVP_CipherFinal_ex(c, out + n, &m);
    EVP_CIPHER_CTX_free(c);
    return ok && (size_t)(n + m) == len ? ESP_OK : ESP_FAIL;
}
esp_err_t nfc_aes_cbc_encrypt(const uint8_t key[16], const uint8_t iv[16], const uint8_t *in, size_t len, uint8_t *out)
{
    return cbc(1, key, iv, in, len, out);
}
esp_err_t nfc_aes_cbc_decrypt(const uint8_t key[16], const uint8_t iv[16], const uint8_t *in, size_t len, uint8_t *out)
{
    return cbc(0, key, iv, in, len, out);
}
esp_err_t nfc_aes_cmac(const uint8_t key[16], const uint8_t *data, size_t len, uint8_t mac[16])
{
    if (host_cmac_fail) {
        memset(mac, 0, 16);
        return ESP_FAIL;
    }
    EVP_MAC *m = EVP_MAC_fetch(NULL, "CMAC", NULL); EVP_MAC_CTX *c = EVP_MAC_CTX_new(m);
    OSSL_PARAM p[] = { OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_CIPHER, "AES-128-CBC", 0), OSSL_PARAM_construct_end() };
    size_t out = 0;
    int ok = EVP_MAC_init(c, key, 16, p) && EVP_MAC_update(c, data, len) && EVP_MAC_final(c, mac, &out, 16);
    EVP_MAC_CTX_free(c); EVP_MAC_free(m);
    return ok && out == 16 ? ESP_OK : ESP_FAIL;
}
#include <openssl/crypto.h>
int mbedtls_ct_memcmp(const void *a, const void *b, size_t n)
{
    return CRYPTO_memcmp(a, b, n);
}
