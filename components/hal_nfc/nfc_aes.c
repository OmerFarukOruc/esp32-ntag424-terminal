/*
 * SPDX-FileCopyrightText: 2025-2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file nfc_aes.c
 * @brief AES-128 CBC and CMAC through the PSA Crypto API
 *
 * CBC uses the multipart API so the caller picks the IV; the one-shot call
 * generates a random IV, which the NTAG424 protocol cannot use. Each call
 * imports a volatile key and destroys it before it returns.
 */

#include <string.h>
#include "hal_nfc_priv.h"
#include "psa/crypto.h"

static const char *TAG = "hal_nfc";

#define AES_KEY_BITS    128
#define AES_BLOCK_LEN   16

static esp_err_t key_import(const uint8_t key[16], psa_key_usage_t usage,
                            psa_algorithm_t alg, psa_key_id_t *key_id)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, AES_KEY_BITS);
    psa_set_key_usage_flags(&attr, usage);
    psa_set_key_algorithm(&attr, alg);

    psa_status_t status = psa_import_key(&attr, key, AES_BLOCK_LEN, key_id);
    psa_reset_key_attributes(&attr);
    ESP_RETURN_ON_FALSE(status == PSA_SUCCESS, ESP_FAIL, TAG, "key import failed: %d", (int)status);
    return ESP_OK;
}

static esp_err_t aes_cbc(const uint8_t key[16], const uint8_t iv[16],
                         const uint8_t *in, size_t len, uint8_t *out, bool encrypt)
{
    psa_key_id_t key_id = PSA_KEY_ID_NULL;
    psa_cipher_operation_t op = PSA_CIPHER_OPERATION_INIT;
    size_t out_len = 0, finish_len = 0;

    memset(out, 0, len);
    ESP_RETURN_ON_FALSE(len != 0 && len % AES_BLOCK_LEN == 0, ESP_ERR_INVALID_SIZE, TAG,
                        "CBC input length %u is not a block multiple", (unsigned)len);
    ESP_RETURN_ON_ERROR(key_import(key, encrypt ? PSA_KEY_USAGE_ENCRYPT : PSA_KEY_USAGE_DECRYPT,
                                   PSA_ALG_CBC_NO_PADDING, &key_id), TAG, "CBC key import");

    psa_status_t status = encrypt ? psa_cipher_encrypt_setup(&op, key_id, PSA_ALG_CBC_NO_PADDING) :
                          psa_cipher_decrypt_setup(&op, key_id, PSA_ALG_CBC_NO_PADDING);
    if (status == PSA_SUCCESS) {
        status = psa_cipher_set_iv(&op, iv, AES_BLOCK_LEN);
    }
    if (status == PSA_SUCCESS) {
        status = psa_cipher_update(&op, in, len, out, len, &out_len);
    }
    if (status == PSA_SUCCESS) {
        status = psa_cipher_finish(&op, out + out_len, len - out_len, &finish_len);
    }
    psa_cipher_abort(&op);
    psa_destroy_key(key_id);

    if (status != PSA_SUCCESS || out_len + finish_len != len) {
        ESP_LOGE(TAG, "CBC %s failed: status=%d len=%u+%u of %u", encrypt ? "encrypt" : "decrypt",
                 (int)status, (unsigned)out_len, (unsigned)finish_len, (unsigned)len);
        memset(out, 0, len);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t nfc_aes_cbc_encrypt(const uint8_t key[16], const uint8_t iv[16],
                              const uint8_t *in, size_t len, uint8_t *out)
{
    return aes_cbc(key, iv, in, len, out, true);
}

esp_err_t nfc_aes_cbc_decrypt(const uint8_t key[16], const uint8_t iv[16],
                              const uint8_t *in, size_t len, uint8_t *out)
{
    return aes_cbc(key, iv, in, len, out, false);
}

esp_err_t nfc_aes_cmac(const uint8_t key[16], const uint8_t *data, size_t len, uint8_t mac[16])
{
    psa_key_id_t key_id = PSA_KEY_ID_NULL;
    size_t mac_len = 0;

    memset(mac, 0, AES_BLOCK_LEN);
    ESP_RETURN_ON_ERROR(key_import(key, PSA_KEY_USAGE_SIGN_MESSAGE, PSA_ALG_CMAC, &key_id),
                        TAG, "CMAC key import");

    psa_status_t status = psa_mac_compute(key_id, PSA_ALG_CMAC, data, len, mac, AES_BLOCK_LEN, &mac_len);
    psa_destroy_key(key_id);

    if (status != PSA_SUCCESS || mac_len != AES_BLOCK_LEN) {
        ESP_LOGE(TAG, "CMAC compute failed: status=%d mac_len=%u", (int)status, (unsigned)mac_len);
        memset(mac, 0, AES_BLOCK_LEN);
        return ESP_FAIL;
    }
    return ESP_OK;
}
