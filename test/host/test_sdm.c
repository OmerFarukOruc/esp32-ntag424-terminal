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

#include "check.h"
#include "host.h"

#define VENDOR_NXP 0x04
#define TYPE_NTAG 0x04
#define MAJOR_NTAG424 0x30
#define ACCESS_CHANGE_KEY_0 0xE0
#define ACCESS_CHANGE_KEY_F 0xEF
#define WRITE_DATA_OFFSET 12
#define SETTINGS_OFFSET 6
#define SETTINGS_LEN 15

static int64_t deadline_us;
static int detect_calls;
static int success_detect_call;
static int late_commands;
static int commands;
static int writes;
static int write_order;
static int settings;
static int settings_order;
static int selects;
static int selects_on_inactive;
static uint32_t recovery_ms;
static uint8_t file_access0;
static int recover_reinits;
static int select_fails_left;
static bool tag_active;
static int version_frame;
static uint8_t hw_vendor = VENDOR_NXP;
static uint8_t hw_type = TYPE_NTAG;
static uint8_t hw_major = MAJOR_NTAG424;
static uint8_t written[NFC_MAX_NDEF_LEN];
static size_t written_len;
static uint8_t settings_block[SETTINGS_LEN];

pn532_t *nfc_reader_open(void)
{
    return NULL;
}

bool nfc_reader_recover(bool *re)
{
    if (re) {
        *re = recover_reinits;
    }
    if (recover_reinits) {
        tag_active = false;
    }
    host_now_us += (int64_t)recovery_ms * 1000;
    return true;
}

hal_nfc_err_t nfc_reader_detect_tag(uint32_t ms)
{
    if (++detect_calls < success_detect_call) {
        host_now_us += (int64_t)ms * 1000;
        return HAL_NFC_ERR_NO_TAG;
    }
    host_now_us += (int64_t)(ms - 100) * 1000;
    tag_active = true;
    return HAL_NFC_OK;
}

bool nfc_reader_tag_is_iso_dep(void)
{
    return true;
}

uint8_t *pn532_ats(pn532_t *r)
{
    static uint8_t ats[] = { 1 };
    return ats;
}

void nfc_reader_release_tag(void) {}
void pn532_io_deadline_set(uart_port_t p, int64_t d)
{
}

void pn532_io_deadline_clear(void) {}
esp_err_t nfc_aes_cbc_encrypt(const uint8_t k[16], const uint8_t iv[16], const uint8_t *in, size_t n, uint8_t *out)
{
    memmove(out, in, n);
    return ESP_OK;
}

esp_err_t nfc_aes_cbc_decrypt(const uint8_t k[16], const uint8_t iv[16], const uint8_t *in, size_t n, uint8_t *out)
{
    memmove(out, in, n);
    return ESP_OK;
}

esp_err_t nfc_aes_cmac(const uint8_t k[16], const uint8_t *d, size_t n, uint8_t mac[16])
{
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
    buf[n] = sw1;
    buf[n + 1] = sw2;
    return n + 2;
}

int pn532_dx(void *p, unsigned int len, uint8_t *buf, unsigned int max, const char **errstr)
{
    commands++;
    if (host_now_us >= deadline_us) {
        late_commands++;
    }
    static const uint8_t zeros[32];
    uint8_t ins = buf[1];
    if (buf[0] == 0x00 && ins == 0xA4) {
        host_now_us += 200000;
        selects++;
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
        return answer(buf, zeros, 16, 0x91, 0xAF);
    case 0x60: {
        host_now_us += 20000;
        version_frame = 1;
        const uint8_t hw[7] = { hw_vendor, hw_type, 0x02, hw_major, 0x00, 0x11, 0x05 };
        return answer(buf, hw, 7, 0x91, 0xAF);
    }
    case 0xAF: {
        if (version_frame == 1) {
            host_now_us += 20000;
            version_frame = 2;
            const uint8_t sw[7] = { hw_vendor, hw_type, 0x02, 0x01, 0x02, 0x11, 0x05 };
            return answer(buf, sw, 7, 0x91, 0xAF);
        }
        if (version_frame == 2) {
            host_now_us += 20000;
            version_frame = 0;
            return answer(buf, zeros, 14, 0x91, 0x00);
        }
        host_now_us += 200000;
        uint8_t r[32] = { 0x9D, 0x00, 0xC4, 0xDF };
        const uint8_t *rnda = &buf[5];
        memcpy(&r[4], rnda + 1, 15);
        r[19] = rnda[0];
        return answer(buf, r, 32, 0x91, 0x00);
    }
    case 0x8D:
        host_now_us += 200000;
        writes++;
        write_order = commands;
        written_len = buf[9] | (buf[10] << 8) | ((size_t)buf[11] << 16);
        memcpy(written, &buf[WRITE_DATA_OFFSET], written_len);
        return answer(buf, NULL, 0, 0x91, 0x00);
    case 0x5F:
        host_now_us += 200000;
        settings++;
        settings_order = commands;
        memcpy(settings_block, &buf[SETTINGS_OFFSET], SETTINGS_LEN);
        if ((file_access0 & 0x0F) != 0) {
            return answer(buf, NULL, 0, 0x91, 0x9D);
        }
        return answer(buf, zeros, 8, 0x91, 0x00);
    }
    *errstr = "unexpected command";
    return -1;
}

static const char EXPECTED_URL[] =
    "example.com/tap/TEST123?picc_data=00000000000000000000000000000000&cmac=0000000000000000";
static const uint8_t EXPECTED_NDEF_HEADER[] = { 0x00, 0x5D, 0xD1, 0x01, 0x59, 0x55, 0x04 };
static const uint8_t EXPECTED_SETTINGS[SETTINGS_LEN] = {
    0x40, 0x00, 0xE0, 0xC1, 0xFF, 0x22, 0x29, 0x00, 0x00, 0x4F, 0x00, 0x00, 0x4F, 0x00, 0x00
};

static hal_nfc_err_t run(int succeed_on, uint32_t recover_ms, int reinits, int select_fails, uint8_t access0)
{
    host_now_us += 1000000;
    detect_calls = 0;
    late_commands = 0;
    commands = 0;
    writes = 0;
    write_order = 0;
    settings = 0;
    settings_order = 0;
    selects = 0;
    selects_on_inactive = 0;
    written_len = 0;
    memset(settings_block, 0, sizeof settings_block);
    success_detect_call = succeed_on;
    recovery_ms = recover_ms;
    recover_reinits = reinits;
    select_fails_left = select_fails;
    file_access0 = access0;
    tag_active = false;
    version_frame = 0;
    g_nfc.pn532 = (pn532_t *)1;
    if (g_nfc.mutex == NULL) {
        g_nfc.mutex = xSemaphoreCreateMutex();
    }
    deadline_us = host_now_us + 30000000;
    return hal_nfc_ntag424_configure_sdm("TEST123", 30000);
}

static void hex_text(const uint8_t *b, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) {
        sprintf(out + 2 * i, "%02X", b[i]);
    }
}

int main(void)
{
    char d[300];
    struct {
        const char *label;
        int succeed_on;
        uint32_t recover_ms;
    } timing[] = {
        { "5 s preflight, tag on 2nd detect", 2, 5000 },
        { "no preflight, tag on 4th detect", 4, 0 },
    };
    for (size_t i = 0; i < sizeof timing / sizeof timing[0]; i++) {
        int64_t t0 = host_now_us + 1000000;
        hal_nfc_err_t r = run(timing[i].succeed_on, timing[i].recover_ms, 0, 0, ACCESS_CHANGE_KEY_0);
        snprintf(d, sizeof d, "result=%d elapsed=%lld ms commands after deadline=%d writes=%d settings=%d", r,
                 (long long)((host_now_us - t0) / 1000), late_commands, writes, settings);
        char name[120];
        snprintf(name, sizeof name, "%s: times out with no tag command after 30 s, done by 30.2 s", timing[i].label);
        check(name, r == HAL_NFC_ERR_SDM_TIMEOUT && late_commands == 0 && host_now_us - t0 <= 30200000, d);
    }

    hal_nfc_err_t r = run(1, 0, 0, 0, ACCESS_CHANGE_KEY_0);
    bool url_ok = written_len == sizeof EXPECTED_NDEF_HEADER + strlen(EXPECTED_URL) &&
                  memcmp(written, EXPECTED_NDEF_HEADER, sizeof EXPECTED_NDEF_HEADER) == 0 &&
                  memcmp(written + sizeof EXPECTED_NDEF_HEADER, EXPECTED_URL, strlen(EXPECTED_URL)) == 0;
    char got[2 * SETTINGS_LEN + 1];
    hex_text(settings_block, SETTINGS_LEN, got);
    snprintf(d, sizeof d, "result=%d, WriteData #%d of %zu bytes matches=%d, ChangeFileSettings #%d block %s", r,
             write_order, written_len, url_ok, settings_order, got);
    check("a configured tag gets the SDM URL by WriteData, then the SDM file settings",
          r == HAL_NFC_OK && writes == 1 && settings == 1 && write_order < settings_order && url_ok &&
          memcmp(settings_block, EXPECTED_SETTINGS, SETTINGS_LEN) == 0, d);

    r = hal_nfc_ntag424_configure_sdm("TEST123", 0);
    snprintf(d, sizeof d, "result=%d", r);
    check("timeout_ms 0 is refused", r == HAL_NFC_ERR_INVALID_PARAM, d);

    r = run(1, 0, 1, 1, ACCESS_CHANGE_KEY_0);
    snprintf(d, sizeof d, "result=%d detects=%d selects=%d on a fresh PN532=%d writes=%d", r, detect_calls, selects,
             selects_on_inactive, writes);
    check("a SELECT retry after a PN532 re-init detects the tag again first", selects_on_inactive == 0 && r == HAL_NFC_OK, d);

    r = run(1, 0, 0, 0, ACCESS_CHANGE_KEY_F);
    snprintf(d, sizeof d, "result=%d writes=%d settings=%d", r, writes, settings);
    check("a file 02 that key 0 may not change is refused before WriteData", writes == 0 && r == HAL_NFC_ERR_NOT_SUPPORTED, d);

    struct {
        const char *name;
        uint8_t vendor;
        uint8_t type;
        uint8_t major;
    } not_424[] = {
        { "a tag from another vendor is refused before WriteData", 0x05, TYPE_NTAG, MAJOR_NTAG424 },
        { "a tag that is not an NTAG is refused before WriteData", VENDOR_NXP, 0x01, MAJOR_NTAG424 },
        { "an NTAG of another hardware version is refused before WriteData", VENDOR_NXP, TYPE_NTAG, 0x12 },
    };
    for (size_t i = 0; i < sizeof not_424 / sizeof not_424[0]; i++) {
        hw_vendor = not_424[i].vendor;
        hw_type = not_424[i].type;
        hw_major = not_424[i].major;
        r = run(1, 0, 0, 0, ACCESS_CHANGE_KEY_0);
        snprintf(d, sizeof d, "version %02X %02X %02X: result=%d writes=%d", hw_vendor, hw_type, hw_major, r, writes);
        check(not_424[i].name, writes == 0 && r == HAL_NFC_ERR_NOT_SUPPORTED, d);
    }
    hw_vendor = VENDOR_NXP;
    hw_type = TYPE_NTAG;
    hw_major = MAJOR_NTAG424;

    struct {
        const char *id;
        bool valid;
    } ids[] = {
        { "Shop_01-a", true },
        { "abcdefghijklmnopqrstuvwxyz01234", true },
        { "abcdefghijklmnopqrstuvwxyz012345", false },
        { "", false },
        { "a/b", false },
        { "a?b", false },
        { "a%b", false },
        { "a b", false },
        { "a.b", false },
    };
    int wrong = 0;
    d[0] = '\0';
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        if (hal_nfc_device_id_valid(ids[i].id, strlen(ids[i].id)) != ids[i].valid) {
            wrong++;
            snprintf(d + strlen(d), sizeof d - strlen(d), "\"%s\" ", ids[i].id);
        }
    }
    check("device ids of 1 to 31 characters from A-Z a-z 0-9 _ - are accepted and nothing else", wrong == 0,
          wrong ? d : "all 9 ids judged as expected");
    return check_summary();
}
