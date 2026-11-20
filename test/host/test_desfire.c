/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/* Response MAC vector from AN12196: KSesAuthMAC 4C66.., TI 9D00C4DF, CmdCtr 1, MAC 57BFF87B1241E93D. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include DESFIRE_SRC
#include "check.h"

extern int host_cmac_fail;

#define STATUS_OK_LEN 2

static const char AN12196_MAC_KEY[] = "4C6626F5E72EA694202139295C7A7FC7";
static const char AN12196_TI[] = "9D00C4DF";
static const char AN12196_RESPONSE_MAC[] = "57BFF87B1241E93D";

nfc_ctx_t g_nfc;

bool nfc_deadline_passed(void)
{
    return false;
}

static uint8_t answer[64];
static int answer_len;
static int exchanges;

int pn532_dx(void *p, unsigned int len, uint8_t *data, unsigned int max, const char **errstr)
{
    exchanges++;
    if (answer_len < 0) {
        *errstr = "scripted failure";
        return -1;
    }
    memcpy(data, answer, (size_t)answer_len);
    return answer_len;
}

static void hex(const char *h, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        sscanf(h + 2 * i, "%2hhx", &out[i]);
    }
}

static void seed(const uint8_t mac_key[16])
{
    nfc_ev2_session_reset();
    memcpy(s_session.ses_auth_mac_key, mac_key, 16);
    hex("00112233445566778899AABBCCDDEEFF", s_session.ses_auth_enc_key, 16);
    hex(AN12196_TI, s_session.ti, 4);
    s_session.cmd_ctr = 1;
    s_session.authenticated = true;
}

static void response_mac(const uint8_t key[16], uint16_t cmd_ctr, uint8_t mac[CMD_MAC_LEN])
{
    uint8_t in[7] = { DESFIRE_STATUS_OK, cmd_ctr & 0xFF, cmd_ctr >> 8 };
    uint8_t full[16];
    hex(AN12196_TI, &in[3], 4);
    nfc_aes_cmac(key, in, sizeof in, full);
    cmac_truncate(full, mac);
}

static hal_nfc_err_t cfs_with(const uint8_t mac_key[16], const uint8_t *resp, int resp_len)
{
    seed(mac_key);
    g_nfc.pn532 = (pn532_t *)1;
    exchanges = 0;
    answer_len = resp_len;
    if (resp_len > 0) {
        memcpy(answer, resp, (size_t)resp_len);
    }
    const uint8_t settings[3] = { 0x40, 0x00, 0xE0 };
    return nfc_ev2_change_file_settings(2, settings, sizeof settings);
}

static int with_status(uint8_t *resp, const uint8_t *mac, size_t mac_len, uint8_t sw2)
{
    if (mac_len > 0) {
        memcpy(resp, mac, mac_len);
    }
    resp[mac_len] = 0x91;
    resp[mac_len + 1] = sw2;
    return (int)mac_len + STATUS_OK_LEN;
}

static bool needs_new_auth(void)
{
    const uint8_t data[1] = { 0 };
    exchanges = 0;
    return nfc_ev2_write_file(2, data, sizeof data) == HAL_NFC_ERR_AUTH && exchanges == 0;
}

static void key_whose_mac_ends_in_zero(uint8_t key[16])
{
    uint8_t mac[CMD_MAC_LEN];
    for (int i = 0; i < 0x10000; i++) {
        key[14] = i >> 8;
        key[15] = i & 0xFF;
        response_mac(key, 2, mac);
        if (mac[CMD_MAC_LEN - 1] == 0) {
            return;
        }
    }
    abort();
}

int main(void)
{
    char d[200];
    uint8_t key[16];
    uint8_t mac[CMD_MAC_LEN];
    uint8_t resp[40];
    hex(AN12196_MAC_KEY, key, 16);
    hex(AN12196_RESPONSE_MAC, mac, CMD_MAC_LEN);

    hal_nfc_err_t e = cfs_with(key, resp, with_status(resp, mac, CMD_MAC_LEN, DESFIRE_STATUS_OK));
    snprintf(d, sizeof d, "err=%d cmd_ctr=%u", e, s_session.cmd_ctr);
    check("ChangeFileSettings accepts the AN12196 response MAC and advances the counter to 2",
          e == HAL_NFC_OK && s_session.cmd_ctr == 2, d);

    uint8_t short_key[16];
    uint8_t short_mac[CMD_MAC_LEN];
    hex(AN12196_MAC_KEY, short_key, 16);
    key_whose_mac_ends_in_zero(short_key);
    response_mac(short_key, 2, short_mac);
    e = cfs_with(short_key, resp, with_status(resp, short_mac, CMD_MAC_LEN - 1, DESFIRE_STATUS_OK));
    bool ended = needs_new_auth();
    snprintf(d, sizeof d, "first 7 bytes of a MAC whose 8th byte is 0: err=%d, next command needs authentication=%d", e,
             ended);
    check("a 7-byte response MAC is rejected and ends the session", e == HAL_NFC_ERR_COMM && ended, d);

    uint8_t long_mac[CMD_MAC_LEN + 1];
    memcpy(long_mac, mac, CMD_MAC_LEN);
    long_mac[CMD_MAC_LEN] = 0;
    e = cfs_with(key, resp, with_status(resp, long_mac, sizeof long_mac, DESFIRE_STATUS_OK));
    ended = needs_new_auth();
    snprintf(d, sizeof d, "err=%d, next command needs authentication=%d", e, ended);
    check("a 9-byte response is rejected and ends the session", e == HAL_NFC_ERR_COMM && ended, d);

    uint8_t flipped[CMD_MAC_LEN];
    memcpy(flipped, mac, CMD_MAC_LEN);
    flipped[CMD_MAC_LEN - 1] ^= 1;
    e = cfs_with(key, resp, with_status(resp, flipped, CMD_MAC_LEN, DESFIRE_STATUS_OK));
    ended = needs_new_auth();
    snprintf(d, sizeof d, "err=%d, next command needs authentication=%d", e, ended);
    check("a response MAC with one flipped bit is rejected and ends the session", e == HAL_NFC_ERR_COMM && ended, d);

    uint8_t old_ctr_mac[CMD_MAC_LEN];
    response_mac(key, 1, old_ctr_mac);
    e = cfs_with(key, resp, with_status(resp, old_ctr_mac, CMD_MAC_LEN, DESFIRE_STATUS_OK));
    ended = needs_new_auth();
    snprintf(d, sizeof d, "err=%d, next command needs authentication=%d", e, ended);
    check("a response MAC over the old counter is rejected and ends the session", e == HAL_NFC_ERR_COMM && ended, d);

    e = cfs_with(key, resp, with_status(resp, mac, CMD_MAC_LEN, DESFIRE_STATUS_AUTH_ERR));
    snprintf(d, sizeof d, "err=%d", e);
    check("status 91 AE maps to HAL_NFC_ERR_AUTH", e == HAL_NFC_ERR_AUTH, d);

    e = cfs_with(key, NULL, -1);
    ended = needs_new_auth();
    snprintf(d, sizeof d, "err=%d, next command needs authentication=%d", e, ended);
    check("a PN532 failure during ChangeFileSettings ends the session", e == HAL_NFC_ERR_COMM && ended, d);

    seed(key);
    answer_len = with_status(answer, NULL, 0, DESFIRE_STATUS_OK);
    uint8_t ndef[4] = { 1, 2, 3, 4 };
    e = nfc_ev2_write_file(2, ndef, sizeof ndef);
    snprintf(d, sizeof d, "err=%d cmd_ctr=%u", e, s_session.cmd_ctr);
    check("a plain WriteData advances the command counter", e == HAL_NFC_OK && s_session.cmd_ctr == 2, d);

    host_cmac_fail = 1;
    e = cfs_with(key, resp, with_status(resp, mac, CMD_MAC_LEN, DESFIRE_STATUS_OK));
    int sent = exchanges;
    host_cmac_fail = 0;
    ended = needs_new_auth();
    snprintf(d, sizeof d, "command MAC fails: err=%d, exchanges=%d, next command needs authentication=%d", e, sent,
             ended);
    check("a ChangeFileSettings that fails before sending ends the session",
          e == HAL_NFC_ERR_COMM && sent == 0 && ended, d);
    return check_summary();
}
