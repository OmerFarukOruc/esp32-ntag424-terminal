/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/* Response MAC vector: AN12196 Table 18 (KSesAuthMAC 4C66.., TI 9D00C4DF, CmdCtr 1, MAC 57BFF87B1241E93D). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include DESFIRE_SRC

nfc_ctx_t g_nfc;
bool nfc_deadline_passed(void)
{
    return false;
}
void esp_fill_random(void *buf, size_t len)
{
    memset(buf, 0x11, len);
}
pn532_err_t nfc_reader_last_error(void)
{
    return PN532_ERR_NULL;
}
hal_nfc_err_t nfc_reader_err_to_hal(pn532_err_t e)
{
    (void)e;
    return HAL_NFC_ERR_COMM;
}

static uint8_t answer[64]; static int answer_len; static int exchanges;
int pn532_dx(void *p, unsigned int len, uint8_t *data, unsigned int max, const char **errstr)
{
    (void)p; (void)len; (void)max; exchanges++;
    if (answer_len < 0) {
        *errstr = "scripted failure";
        return -1;
    }
    memcpy(data, answer, (size_t)answer_len); return answer_len;
}
extern int host_cmac_fail;

#include "check.h"
static void hex(const char *h, uint8_t *out, int n)
{
    for (int i = 0; i < n; i++) {
        sscanf(h + 2 * i, "%2hhx", &out[i]);
    }
}
static void seed(void)
{
    nfc_ev2_session_reset();
    hex("4C6626F5E72EA694202139295C7A7FC7", s_session.ses_auth_mac_key, 16);
    hex("00112233445566778899AABBCCDDEEFF", s_session.ses_auth_enc_key, 16);
    hex("9D00C4DF", s_session.ti, 4);
    s_session.cmd_ctr = 1; s_session.authenticated = true;
}
static hal_nfc_err_t cfs_with(const char *resp_hex)
{
    seed(); g_nfc.pn532 = (pn532_t *)1; exchanges = 0;
    if (resp_hex == NULL) {
        answer_len = -1;
    } else {
        answer_len = (int)strlen(resp_hex) / 2;
        hex(resp_hex, answer, answer_len);
    }
    const uint8_t settings[3] = { 0x40, 0x00, 0xE0 };
    return nfc_ev2_change_file_settings(2, settings, sizeof settings);
}
static bool needs_new_auth(void)
{
    const uint8_t data[1] = { 0 };
    exchanges = 0;
    return nfc_ev2_write_file(2, data, sizeof data) == HAL_NFC_ERR_AUTH && exchanges == 0;
}

int main(void)
{
    char d[200];
    uint8_t key[16], full[16], t[8], in[7] = { 0x00, 0x01, 0x00, 0x9D, 0x00, 0xC4, 0xDF };
    hex("4C6626F5E72EA694202139295C7A7FC7", key, 16);

    hal_nfc_err_t e = cfs_with("57BFF87B1241E93D9100");
    snprintf(d, sizeof d, "err=%d cmd_ctr=%u auth=%d", e, s_session.cmd_ctr, s_session.authenticated);
    check("ChangeFileSettings accepts the AN12196 response MAC and advances the counter to 2", e == HAL_NFC_OK && s_session.cmd_ctr == 2, d);

    char old_ctr_resp[21];
    nfc_aes_cmac(key, in, 7, full); cmac_truncate(full, t);
    for (int i = 0; i < 8; i++) {
        sprintf(old_ctr_resp + 2 * i, "%02X", t[i]);
    }
    strcpy(old_ctr_resp + 16, "9100");
    struct {
        const char *name, *resp;
    } bad[] = {
        { "ChangeFileSettings without a response MAC is rejected and ends the session", "9100" },
        { "a 7-byte response MAC is rejected and ends the session", "57BFF87B1241E99100" },
        { "a 9-byte response is rejected and ends the session", "57BFF87B1241E93D009100" },
        { "a response MAC with one flipped bit is rejected and ends the session", "57BFF87B1241E93C9100" },
        { "a response MAC over the old counter is rejected and ends the session", old_ctr_resp },
        { "a 32-byte response is rejected and ends the session", "57BFF87B1241E93D57BFF87B1241E93D57BFF87B1241E93D57BFF87B1241E93D9100" },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        e = cfs_with(bad[i].resp);
        bool ended = needs_new_auth();
        snprintf(d, sizeof d, "err=%d, next command needs authentication=%d", e, ended);
        check(bad[i].name, e == HAL_NFC_ERR_COMM && ended, d);
    }
    e = cfs_with("57BFF87B1241E93D91AE");
    snprintf(d, sizeof d, "err=%d auth=%d", e, s_session.authenticated);
    check("status 91 AE maps to HAL_NFC_ERR_AUTH", e == HAL_NFC_ERR_AUTH, d);
    e = cfs_with(NULL);
    bool ended = needs_new_auth();
    snprintf(d, sizeof d, "err=%d, next command needs authentication=%d", e, ended);
    check("a PN532 failure during ChangeFileSettings ends the session", e != HAL_NFC_OK && ended, d);

    seed(); answer_len = 2; answer[0] = 0x91; answer[1] = 0x00;
    exchanges = 0;
    uint8_t ndef[4] = { 1, 2, 3, 4 };
    e = nfc_ev2_write_file(2, ndef, sizeof ndef);
    snprintf(d, sizeof d, "err=%d cmd_ctr=%u", e, s_session.cmd_ctr);
    check("a plain WriteData advances the command counter", e == HAL_NFC_OK && s_session.cmd_ctr == 2, d);

    host_cmac_fail = 1;
    e = cfs_with("57BFF87B1241E93D9100");
    int sent = exchanges;
    host_cmac_fail = 0;
    ended = needs_new_auth();
    snprintf(d, sizeof d, "command MAC fails: err=%d, exchanges=%d, next command needs authentication=%d", e, sent, ended);
    check("a ChangeFileSettings that fails before sending ends the session", e == HAL_NFC_ERR_COMM && sent == 0 && ended, d);
    return check_summary();
}
