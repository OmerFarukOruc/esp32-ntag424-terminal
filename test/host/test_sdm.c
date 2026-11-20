/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/uart.h"
#include "hal_nfc_priv.h"

extern int64_t host_now_us;
static int64_t deadline_us;
static int detect_calls, success_detect_call, late_commands, writes, settings, selects, selects_on_inactive;
static uint32_t recovery_ms;
static uint8_t file_access0 = 0xE0;   /* RW = E, Change = 0, as on a new tag */
static int recover_reinits, select_fails_left, tag_active, version_frame;
static uint8_t hw_type = 0x04, hw_major = 0x30;   /* NTAG, NTAG 424 DNA */

pn532_t *nfc_reader_open(void)
{
    return NULL;
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
bool nfc_reader_recover(bool *re)
{
    if (re) {
        *re = recover_reinits;
    }
    if (recover_reinits) {
        tag_active = 0;
    }
    host_now_us += (int64_t)recovery_ms * 1000; return true;
}
hal_nfc_err_t nfc_reader_detect_tag(uint32_t ms)
{
    if (++detect_calls < success_detect_call) {
        host_now_us += (int64_t)ms * 1000;
        return HAL_NFC_ERR_NO_TAG;
    }
    host_now_us += (int64_t)(ms - 100) * 1000; tag_active = 1; return HAL_NFC_OK;
}
bool nfc_reader_tag_is_iso_dep(void)
{
    return true;
}
uint8_t *pn532_ats(pn532_t *r)
{
    (void)r;
    static uint8_t ats[] = { 1 };
    return ats;
}
void nfc_reader_release_tag(void) {}
void pn532_io_deadline_set(uart_port_t p, int64_t d)
{
    (void)p;
    (void)d;
}
void pn532_io_deadline_clear(void) {}
esp_err_t nfc_aes_cbc_encrypt(const uint8_t k[16], const uint8_t iv[16], const uint8_t *in, size_t n, uint8_t *out)
{
    (void)k;
    (void)iv;
    memmove(out, in, n);
    return ESP_OK;
}
esp_err_t nfc_aes_cbc_decrypt(const uint8_t k[16], const uint8_t iv[16], const uint8_t *in, size_t n, uint8_t *out)
{
    (void)k;
    (void)iv;
    memmove(out, in, n);
    return ESP_OK;
}
esp_err_t nfc_aes_cmac(const uint8_t k[16], const uint8_t *d, size_t n, uint8_t mac[16])
{
    (void)k;
    (void)d;
    (void)n;
    memset(mac, 0, 16);
    return ESP_OK;
}
int mbedtls_ct_memcmp(const void *a, const void *b, size_t n)
{
    return memcmp(a, b, n);
}

static int answer(uint8_t *buf, const uint8_t *data, int n, uint8_t sw1, uint8_t sw2)
{
    if (n) {
        memcpy(buf, data, n);
    }
    buf[n] = sw1; buf[n + 1] = sw2; return n + 2;
}
int pn532_dx(void *p, unsigned int len, uint8_t *buf, unsigned int max, const char **errstr)
{
    (void)p; (void)len; (void)max;
    if (host_now_us >= deadline_us) {
        late_commands++;
    }
    static const uint8_t zeros[32];
    uint8_t ins = buf[1];
    if (buf[0] == 0x00 && ins == 0xA4) {
        host_now_us += 200000; selects++;
        if (!tag_active) {
            selects_on_inactive++;
            *errstr = "no target";
            return -1;
        }
        if (select_fails_left > 0) {
            select_fails_left--;
            *errstr = "protocol error";
            return -1;
        }
        return answer(buf, NULL, 0, 0x90, 0x00);
    }
    switch (ins) {
    case 0xF5: {
        host_now_us += 50000;
        const uint8_t fs[7] = { 0x00, 0x00, file_access0, 0xEE, 0x00, 0x01, 0x00 };
        return answer(buf, fs, 7, 0x91, 0x00);
    }
    case 0x71:
        host_now_us += 200000;
        return answer(buf, zeros, 16, 0x91, 0xAF);   /* EncRndB; identity AES makes RndB all zero */
    case 0x60: {
        host_now_us += 20000; version_frame = 1;
        const uint8_t hw[7] = { 0x04, hw_type, 0x02, hw_major, 0x00, 0x11, 0x05 };
        return answer(buf, hw, 7, 0x91, 0xAF);
    }
    case 0xAF: {
        if (version_frame == 1) {
            host_now_us += 20000;
            version_frame = 2;
            const uint8_t sw[7] = { 0x04, hw_type, 0x02, 0x01, 0x02, 0x11, 0x05 };
            return answer(buf, sw, 7, 0x91, 0xAF);
        }
        if (version_frame == 2) {
            host_now_us += 20000;
            version_frame = 0;
            return answer(buf, zeros, 14, 0x91, 0x00);
        }
        host_now_us += 200000;
        uint8_t r[32] = { 0x9D, 0x00, 0xC4, 0xDF };   /* TI, RndA rotated left, PDcap2, PCDcap2 */
        const uint8_t *rnda = &buf[5];
        memcpy(&r[4], rnda + 1, 15); r[19] = rnda[0];
        return answer(buf, r, 32, 0x91, 0x00);
    }
    case 0x8D:
        host_now_us += 200000; writes++;
        return answer(buf, NULL, 0, 0x91, 0x00);
    case 0x5F:
        host_now_us += 200000; settings++;
        if ((file_access0 & 0x0F) != 0) {
            return answer(buf, NULL, 0, 0x91, 0x9D);
        }
        return answer(buf, zeros, 8, 0x91, 0x00);
    }
    *errstr = "unexpected command";
    return -1;
}

#include "check.h"
static hal_nfc_err_t run(int succeed_on, uint32_t recover_ms, int reinits, int select_fails, uint8_t access0)
{
    host_now_us += 1000000;
    detect_calls = late_commands = writes = settings = selects = selects_on_inactive = 0;
    success_detect_call = succeed_on; recovery_ms = recover_ms; recover_reinits = reinits;
    select_fails_left = select_fails; file_access0 = access0; tag_active = 0; version_frame = 0;
    g_nfc.pn532 = (pn532_t *)1; if (!g_nfc.mutex) g_nfc.mutex = xSemaphoreCreateMutex();
    deadline_us = host_now_us + 30000000;
    return hal_nfc_ntag424_configure_sdm("TEST123", 30000);
}
int main(void)
{
    char d[200];
    struct {
        const char *label;
        int succeed_on;
        uint32_t recover_ms;
    } f10[] = {
        { "5 s preflight, tag on 2nd detect", 2, 5000 },
        { "no preflight, tag on 4th detect", 4, 0 },
        { "tag on the 1st detect", 1, 0 },
    };
    for (int i = 0; i < 3; i++) {
        int64_t t0 = host_now_us + 1000000;
        hal_nfc_err_t r = run(f10[i].succeed_on, f10[i].recover_ms, 0, 0, 0xE0);
        snprintf(d, sizeof d, "result=%d elapsed=%lld ms commands after deadline=%d writes=%d settings=%d", r,
                 (long long)((host_now_us - t0) / 1000), late_commands, writes, settings);
        char name[120];
        snprintf(name, sizeof name, "%s: no tag command after 30 s, done by 30.2 s", f10[i].label);
        check(name, late_commands == 0 && host_now_us - t0 <= 30200000 && (r == HAL_NFC_OK) == (settings == 1), d);
    }
    hal_nfc_err_t r = hal_nfc_ntag424_configure_sdm("TEST123", 0);
    snprintf(d, sizeof d, "result=%d", r);
    check("timeout_ms 0 is refused", r == HAL_NFC_ERR_INVALID_PARAM, d);

    r = run(1, 0, 1, 1, 0xE0);
    snprintf(d, sizeof d, "result=%d detects=%d selects=%d on a fresh PN532=%d writes=%d", r, detect_calls, selects,
             selects_on_inactive, writes);
    check("a SELECT retry after a PN532 re-init detects the tag again first", selects_on_inactive == 0 && r == HAL_NFC_OK, d);

    r = run(1, 0, 0, 0, 0xEF);
    snprintf(d, sizeof d, "result=%d writes=%d settings=%d", r, writes, settings);
    check("a file 02 that key 0 may not change is refused before WriteData", writes == 0 && r == HAL_NFC_ERR_NOT_SUPPORTED, d);
    hw_type = 0x01; hw_major = 0x12;
    r = run(1, 0, 0, 0, 0xE0);
    hw_type = 0x04; hw_major = 0x30;
    snprintf(d, sizeof d, "DESFire EV2 version answer: result=%d writes=%d settings=%d", r, writes, settings);
    check("a tag that is not an NTAG 424 DNA is refused before WriteData", writes == 0 && r == HAL_NFC_ERR_NOT_SUPPORTED, d);
    return check_summary();
}
