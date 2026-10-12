/*
 * SPDX-FileCopyrightText: 2025-2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file desfire_ev2.c
 * @brief ISO 7816 select and DESFire EV2 secure messaging for NTAG424 DNA
 *
 * Frames travel through the PN532 InDataExchange command. The session state
 * is one static instance; every caller holds g_nfc.mutex.
 */

#include <string.h>
#include "hal_nfc_priv.h"
#include "esp_random.h"

static const char *TAG = "hal_nfc";

/* Native DESFire instruction codes */
#define DESFIRE_INS_AUTH_EV2_FIRST          0x71
#define DESFIRE_INS_ADDITIONAL_FRAME        0xAF
#define DESFIRE_INS_CHANGE_FILE_SETTINGS    0x5F
#define DESFIRE_INS_WRITE_DATA              0x8D

/* DESFire status byte (SW2 after SW1 = 0x91) */
#define DESFIRE_STATUS_OK           0x00
#define DESFIRE_STATUS_MORE         0xAF
#define DESFIRE_STATUS_AUTH_ERR     0xAE

/** Largest ChangeFileSettings block: the padded plaintext fills two AES blocks */
#define CFS_MAX_SETTINGS_LEN        31
#define CFS_MAX_PADDED_LEN          32
#define AES_BLOCK_LEN               16
#define CMD_MAC_LEN                 8

static const uint8_t NDEF_APP_AID[] = {0xD2, 0x76, 0x00, 0x00, 0x85, 0x01, 0x01};
static const uint8_t IV_ZERO[AES_BLOCK_LEN] = {0};

typedef struct {
    uint8_t ti[4];                  /* Transaction Identifier */
    uint8_t ses_auth_enc_key[16];
    uint8_t ses_auth_mac_key[16];
    uint16_t cmd_ctr;
    bool authenticated;
} ev2_session_t;

static ev2_session_t s_session;

void nfc_ev2_session_reset(void)
{
    explicit_bzero(&s_session, sizeof(s_session));
}

/** Exchange one APDU in place; on success *rx_len covers data plus SW1 SW2 */
static hal_nfc_err_t apdu_exchange(uint8_t buf[NFC_APDU_BUF_SIZE], size_t apdu_len, size_t *rx_len)
{
    ESP_RETURN_ON_FALSE(g_nfc.pn532 != NULL, HAL_NFC_ERR_NOT_INIT, TAG, "PN532 not initialized");

    const char *errstr = NULL;
    int result = pn532_dx(g_nfc.pn532, apdu_len, buf, NFC_APDU_BUF_SIZE, &errstr);
    if (result < 0) {
        ESP_LOGE(TAG, "APDU exchange failed: %s", errstr != NULL ? errstr : "unknown");
        return nfc_reader_err_to_hal(nfc_reader_last_error());
    }
    ESP_RETURN_ON_FALSE(result >= 2, HAL_NFC_ERR_COMM, TAG, "APDU response too short: %d", result);

    *rx_len = (size_t)result;
    return HAL_NFC_OK;
}

/** Header, optional Lc and data, then Le = 0; P2 is always 0. Returns the APDU length. */
static size_t build_apdu(uint8_t cla, uint8_t ins, uint8_t p1, const uint8_t *data, size_t data_len,
                         uint8_t buf[NFC_APDU_BUF_SIZE])
{
    size_t n = 0;

    buf[n++] = cla;
    buf[n++] = ins;
    buf[n++] = p1;
    buf[n++] = 0x00;    /* P2 */
    if (data_len > 0) {
        buf[n++] = (uint8_t)data_len;   /* Lc */
        memcpy(&buf[n], data, data_len);
        n += data_len;
    }
    buf[n++] = 0x00;    /* Le */
    return n;
}

hal_nfc_err_t nfc_select_ndef_app(void)
{
    uint8_t buf[NFC_APDU_BUF_SIZE];
    size_t n = build_apdu(0x00, 0xA4, 0x04, NDEF_APP_AID, sizeof(NDEF_APP_AID), buf);   /* SELECT by DF name */

    size_t rx_len = 0;
    hal_nfc_err_t err = apdu_exchange(buf, n, &rx_len);
    if (err != HAL_NFC_OK) {
        ESP_LOGE(TAG, "Failed to select NDEF application");
        return err;
    }

    uint8_t sw1 = buf[rx_len - 2];
    uint8_t sw2 = buf[rx_len - 1];
    if (sw1 == 0x90 && sw2 == 0x00) {
        return HAL_NFC_OK;
    }

    ESP_LOGW(TAG, "SELECT error: SW=%02X%02X", sw1, sw2);
    if (sw1 == 0x6A && sw2 == 0x82) {
        return HAL_NFC_ERR_NOT_SUPPORTED;
    }
    if (sw1 == 0x69 && (sw2 == 0x82 || sw2 == 0x85)) {
        return HAL_NFC_ERR_AUTH;
    }
    return HAL_NFC_ERR_COMM;
}

/**
 * Native DESFire command wrapped in an ISO APDU (CLA 0x90, P1 P2 0). OK only
 * when the DESFire status byte equals expect_sw2, AUTH for 0xAE and COMM for
 * anything else. response and resp_len are both set or both NULL; a longer
 * answer than *resp_len is an error.
 */
static hal_nfc_err_t desfire_send_cmd(uint8_t ins, const uint8_t *data, size_t data_len, uint8_t expect_sw2,
                                      uint8_t *response, size_t *resp_len)
{
    ESP_RETURN_ON_FALSE(data_len <= NFC_DESFIRE_MAX_CMD_DATA, HAL_NFC_ERR_COMM, TAG,
                        "DESFire data of %u bytes does not fit one frame", (unsigned)data_len);
    ESP_RETURN_ON_FALSE((response == NULL) == (resp_len == NULL), HAL_NFC_ERR_COMM, TAG,
                        "DESFire response buffer and length must be set together");

    uint8_t buf[NFC_APDU_BUF_SIZE];
    size_t n = build_apdu(0x90, ins, 0x00, data, data_len, buf);

    size_t rx_len = 0;
    hal_nfc_err_t err = apdu_exchange(buf, n, &rx_len);
    if (err != HAL_NFC_OK) {
        return err;
    }

    uint8_t status_sw1 = buf[rx_len - 2];
    uint8_t status_sw2 = buf[rx_len - 1];
    ESP_RETURN_ON_FALSE(status_sw1 == 0x91, HAL_NFC_ERR_COMM, TAG,
                        "DESFire unexpected SW1: 0x%02X (expected 0x91)", status_sw1);
    if (status_sw2 != expect_sw2) {
        ESP_LOGE(TAG, "DESFire cmd 0x%02X: SW2=0x%02X, expected 0x%02X", ins, status_sw2, expect_sw2);
        return status_sw2 == DESFIRE_STATUS_AUTH_ERR ? HAL_NFC_ERR_AUTH : HAL_NFC_ERR_COMM;
    }

    if (response != NULL) {
        size_t data_out_len = rx_len - 2;
        ESP_RETURN_ON_FALSE(data_out_len <= *resp_len, HAL_NFC_ERR_COMM, TAG,
                            "DESFire answer of %u bytes exceeds the %u byte buffer",
                            (unsigned)data_out_len, (unsigned)*resp_len);
        memcpy(response, buf, data_out_len);
        *resp_len = data_out_len;
    }
    return HAL_NFC_OK;
}

static void rotate_left(const uint8_t in[16], uint8_t out[16])
{
    memcpy(out, in + 1, 15);
    out[15] = in[0];
}

/** Session vector for key derivation: label, counter, length and the RndA/RndB mix */
static void build_session_vector(uint8_t sv[32], uint8_t label0, uint8_t label1,
                                 const uint8_t rnda[16], const uint8_t rndb[16])
{
    sv[0] = label0;
    sv[1] = label1;
    sv[2] = 0x00;
    sv[3] = 0x01;
    sv[4] = 0x00;
    sv[5] = 0x80;
    memcpy(&sv[6], rnda, 2);
    for (int i = 0; i < 6; i++) {
        sv[8 + i] = rnda[2 + i] ^ rndb[i];
    }
    memcpy(&sv[14], &rndb[6], 10);
    memcpy(&sv[24], &rnda[8], 8);
}

typedef struct {
    uint8_t rnda[16];
    uint8_t rndb[16];
    uint8_t plain[32];
    uint8_t cipher[32];
    uint8_t sv[32];
} auth_work_t;

hal_nfc_err_t nfc_ev2_authenticate(uint8_t key_no, const uint8_t key[16])
{
    auth_work_t w;
    uint8_t enc_rndb[16];
    uint8_t resp2[32];
    size_t len = sizeof(enc_rndb);
    hal_nfc_err_t ret = HAL_NFC_OK;
    hal_nfc_err_t err;

    s_session.authenticated = false;

    /* Part 1: KeyNo, LenCap = 0; the tag answers with the encrypted RndB */
    const uint8_t part1[2] = { key_no, 0x00 };
    err = desfire_send_cmd(DESFIRE_INS_AUTH_EV2_FIRST, part1, sizeof(part1), DESFIRE_STATUS_MORE,
                           enc_rndb, &len);
    ESP_RETURN_ON_FALSE(err == HAL_NFC_OK, err, TAG, "AuthEV2First step 1 failed: %d", err);
    ESP_RETURN_ON_FALSE(len == 16, HAL_NFC_ERR_COMM, TAG, "AuthEV2First: expected 16B EncRndB, got %zu", len);

    ESP_GOTO_ON_FALSE(nfc_aes_cbc_decrypt(key, IV_ZERO, enc_rndb, sizeof(enc_rndb), w.rndb) == ESP_OK,
                      HAL_NFC_ERR_COMM, out, TAG, "AuthEV2First: RndB decrypt failed");

    /* Part 2: AES-CBC(key, IV 0) over RndA || RndB rotated left by one byte */
    esp_fill_random(w.rnda, sizeof(w.rnda));
    memcpy(w.plain, w.rnda, 16);
    rotate_left(w.rndb, &w.plain[16]);
    ESP_GOTO_ON_FALSE(nfc_aes_cbc_encrypt(key, IV_ZERO, w.plain, sizeof(w.plain), w.cipher) == ESP_OK,
                      HAL_NFC_ERR_COMM, out, TAG, "AuthEV2First: part 2 encrypt failed");

    len = sizeof(resp2);
    err = desfire_send_cmd(DESFIRE_INS_ADDITIONAL_FRAME, w.cipher, sizeof(w.cipher), DESFIRE_STATUS_OK,
                           resp2, &len);
    ESP_GOTO_ON_FALSE(err == HAL_NFC_OK, err, out, TAG, "AuthEV2First step 2 failed: %d", err);
    ESP_GOTO_ON_FALSE(len == 32, HAL_NFC_ERR_COMM, out, TAG,
                      "AuthEV2First: expected 32B response, got %zu", len);

    /* Response: TI(4) || RndA rotated left(16) || PDcap2(6) || PCDcap2(6) */
    ESP_GOTO_ON_FALSE(nfc_aes_cbc_decrypt(key, IV_ZERO, resp2, sizeof(resp2), w.cipher) == ESP_OK,
                      HAL_NFC_ERR_COMM, out, TAG, "AuthEV2First: response decrypt failed");

    rotate_left(w.rnda, w.plain);
    ESP_GOTO_ON_FALSE(memcmp(&w.cipher[4], w.plain, 16) == 0, HAL_NFC_ERR_AUTH, out, TAG,
                      "AuthEV2First: RndA verification failed");

    memcpy(s_session.ti, w.cipher, 4);
    s_session.cmd_ctr = 0;

    build_session_vector(w.sv, 0xA5, 0x5A, w.rnda, w.rndb);
    ESP_GOTO_ON_FALSE(nfc_aes_cmac(key, w.sv, sizeof(w.sv), s_session.ses_auth_enc_key) == ESP_OK,
                      HAL_NFC_ERR_COMM, out, TAG, "AuthEV2First: encryption key derivation failed");
    build_session_vector(w.sv, 0x5A, 0xA5, w.rnda, w.rndb);
    ESP_GOTO_ON_FALSE(nfc_aes_cmac(key, w.sv, sizeof(w.sv), s_session.ses_auth_mac_key) == ESP_OK,
                      HAL_NFC_ERR_COMM, out, TAG, "AuthEV2First: MAC key derivation failed");

    s_session.authenticated = true;
    ESP_LOGI(TAG, "AuthenticateEV2First successful with Key %d", key_no);

out:
    explicit_bzero(&w, sizeof(w));
    if (ret != HAL_NFC_OK) {
        nfc_ev2_session_reset();
    }
    return ret;
}

hal_nfc_err_t nfc_ev2_write_file(uint8_t file_no, const uint8_t *data, size_t len)
{
    ESP_RETURN_ON_FALSE(s_session.authenticated, HAL_NFC_ERR_AUTH, TAG, "No authenticated session");
    ESP_RETURN_ON_FALSE(data != NULL && len > 0 && len <= NFC_MAX_NDEF_LEN, HAL_NFC_ERR_COMM,
                        TAG, "WriteData length %u is out of range", (unsigned)len);

    uint8_t cmd[NFC_WRITE_DATA_HEADER_LEN + NFC_MAX_NDEF_LEN];
    cmd[0] = file_no;
    cmd[1] = 0x00;          /* offset, 3 bytes LE */
    cmd[2] = 0x00;
    cmd[3] = 0x00;
    cmd[4] = len & 0xFF;    /* length, 3 bytes LE */
    cmd[5] = (len >> 8) & 0xFF;
    cmd[6] = 0x00;
    memcpy(&cmd[NFC_WRITE_DATA_HEADER_LEN], data, len);

    hal_nfc_err_t err = desfire_send_cmd(DESFIRE_INS_WRITE_DATA, cmd, NFC_WRITE_DATA_HEADER_LEN + len,
                                         DESFIRE_STATUS_OK, NULL, NULL);
    if (err == HAL_NFC_OK) {
        s_session.cmd_ctr++;
    }
    return err;
}

/** The odd-indexed bytes of a 16 byte CMAC, as DESFire EV2 truncates it */
static void cmac_truncate(const uint8_t full_mac[AES_BLOCK_LEN], uint8_t short_mac[CMD_MAC_LEN])
{
    for (int i = 0; i < CMD_MAC_LEN; i++) {
        short_mac[i] = full_mac[(i * 2) + 1];
    }
}

/**
 * The response MAC is not verified: the tag state is read from the status
 * bytes alone and the command counter advances on status OK.
 */
hal_nfc_err_t nfc_ev2_change_file_settings(uint8_t file_no, const uint8_t *settings, size_t len)
{
    ESP_RETURN_ON_FALSE(s_session.authenticated, HAL_NFC_ERR_AUTH, TAG, "No authenticated session");
    ESP_RETURN_ON_FALSE(len <= CFS_MAX_SETTINGS_LEN, HAL_NFC_ERR_COMM, TAG,
                        "ChangeFileSettings block of %u bytes does not fit", (unsigned)len);

    /* IV = AES-ECB(KSesAuthENC, A5 5A || TI || CmdCtr LE || 00 x 8); one block, so CBC with IV 0 is ECB */
    uint8_t iv_input[AES_BLOCK_LEN] = {0xA5, 0x5A};
    uint8_t cmd_iv[AES_BLOCK_LEN];
    memcpy(&iv_input[2], s_session.ti, 4);
    iv_input[6] = s_session.cmd_ctr & 0xFF;
    iv_input[7] = (s_session.cmd_ctr >> 8) & 0xFF;
    ESP_RETURN_ON_FALSE(nfc_aes_cbc_encrypt(s_session.ses_auth_enc_key, IV_ZERO, iv_input, sizeof(iv_input),
                                            cmd_iv) == ESP_OK,
                        HAL_NFC_ERR_COMM, TAG, "Command IV derivation failed");

    /* Pad with 0x80 00.. to a block multiple */
    size_t padded_len = ((len / AES_BLOCK_LEN) + 1) * AES_BLOCK_LEN;
    uint8_t padded[CFS_MAX_PADDED_LEN] = {0};
    memcpy(padded, settings, len);
    padded[len] = 0x80;

    /* Command data: CmdHeader || EncryptedData || MAC(8) */
    uint8_t cmd_data[1 + CFS_MAX_PADDED_LEN + CMD_MAC_LEN];
    cmd_data[0] = file_no;
    esp_err_t crypt = nfc_aes_cbc_encrypt(s_session.ses_auth_enc_key, cmd_iv, padded, padded_len, &cmd_data[1]);
    explicit_bzero(padded, sizeof(padded));
    ESP_RETURN_ON_FALSE(crypt == ESP_OK, HAL_NFC_ERR_COMM, TAG, "Command encryption failed");

    /* MAC input: Cmd || CmdCtr LE || TI || CmdHeader || EncryptedData */
    uint8_t mac_input[7 + 1 + CFS_MAX_PADDED_LEN];
    size_t mac_input_len = 0;
    mac_input[mac_input_len++] = DESFIRE_INS_CHANGE_FILE_SETTINGS;
    mac_input[mac_input_len++] = s_session.cmd_ctr & 0xFF;
    mac_input[mac_input_len++] = (s_session.cmd_ctr >> 8) & 0xFF;
    memcpy(&mac_input[mac_input_len], s_session.ti, 4);
    mac_input_len += 4;
    memcpy(&mac_input[mac_input_len], cmd_data, 1 + padded_len);
    mac_input_len += 1 + padded_len;

    uint8_t full_mac[AES_BLOCK_LEN];
    ESP_RETURN_ON_FALSE(nfc_aes_cmac(s_session.ses_auth_mac_key, mac_input, mac_input_len,
                                     full_mac) == ESP_OK,
                        HAL_NFC_ERR_COMM, TAG, "Command MAC failed");
    cmac_truncate(full_mac, &cmd_data[1 + padded_len]);

    hal_nfc_err_t err = desfire_send_cmd(DESFIRE_INS_CHANGE_FILE_SETTINGS, cmd_data,
                                         1 + padded_len + CMD_MAC_LEN, DESFIRE_STATUS_OK, NULL, NULL);
    if (err == HAL_NFC_OK) {
        s_session.cmd_ctr++;
    }
    return err;
}
