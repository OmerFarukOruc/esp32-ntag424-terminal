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

#define UID_A { 7, 0x04, 0x51, 0x2A, 0x6B, 0x1C, 0x7D, 0x80 }
#define UID_B { 7, 0x04, 0x52, 0x2A, 0x6B, 0x1C, 0x7D, 0x81 }
#define TAGS_MAX 4
static uint8_t tag_uids[TAGS_MAX][8];
static int tag_count;
static int tag_now;
static int writes_to[TAGS_MAX];
static uint8_t fs_answer[32];
static size_t fs_answer_len;
static size_t version_len[3];
static uint8_t version_sw2[3];
static int settings_fails_left;
static const uint8_t *production_uid;
static int no_ndef_tag;
static uint32_t run_timeout_ms;

static void tags(int count, const uint8_t (*uids)[8])
{
    tag_count = count;
    memcpy(tag_uids, uids, sizeof tag_uids[0] * count);
}

static void file_settings(const uint8_t *answer, size_t len)
{
    memcpy(fs_answer, answer, len);
    fs_answer_len = len;
}

uint8_t *pn532_nfcid(pn532_t *p, char text[21])
{
    return tag_uids[tag_now];
}

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
    tag_now = detect_calls - success_detect_call < tag_count ? detect_calls - success_detect_call : tag_count - 1;
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
        if (tag_now == no_ndef_tag) {
            return answer(buf, NULL, 0, 0x6A, 0x82);
        }
        return answer(buf, NULL, 0, 0x90, 0x00);
    }
    switch (ins) {
    case 0xF5: {
        host_now_us += 50000;
        if (fs_answer_len > 0) {
            return answer(buf, fs_answer, (int)fs_answer_len, 0x91, 0x00);
        }
        const uint8_t fs[7] = { 0x00, 0x00, file_access0, 0xEE, 0x00, 0x01, 0x00 };
        return answer(buf, fs, 7, 0x91, 0x00);
    }
    case 0x71:
        host_now_us += 200000;
        return answer(buf, zeros, 16, 0x91, 0xAF);
    case 0x60: {
        host_now_us += 20000;
        version_frame = 1;
        const uint8_t hw[8] = { hw_vendor, hw_type, 0x02, hw_major, 0x00, 0x11, 0x05 };
        return answer(buf, hw, (int)version_len[0], 0x91, version_sw2[0]);
    }
    case 0xAF: {
        if (version_frame == 1) {
            host_now_us += 20000;
            version_frame = 2;
            const uint8_t sw[8] = { hw_vendor, hw_type, 0x02, 0x01, 0x02, 0x11, 0x05 };
            return answer(buf, sw, (int)version_len[1], 0x91, version_sw2[1]);
        }
        if (version_frame == 2) {
            host_now_us += 20000;
            version_frame = 0;
            uint8_t production[16] = {0};
            memcpy(production, production_uid != NULL ? production_uid : &tag_uids[tag_now][1], 7);
            return answer(buf, production, (int)version_len[2], 0x91, version_sw2[2]);
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
        writes_to[tag_now]++;
        write_order = commands;
        written_len = buf[9] | (buf[10] << 8) | ((size_t)buf[11] << 16);
        memcpy(written, &buf[WRITE_DATA_OFFSET], written_len);
        return answer(buf, NULL, 0, 0x91, 0x00);
    case 0x5F:
        host_now_us += 200000;
        settings++;
        settings_order = commands;
        memcpy(settings_block, &buf[SETTINGS_OFFSET], SETTINGS_LEN);
        if (settings_fails_left > 0) {
            settings_fails_left--;
            *errstr = "no answer";
            return -1;
        }
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
    memset(writes_to, 0, sizeof writes_to);
    g_nfc.pn532 = (pn532_t *)1;
    if (g_nfc.mutex == NULL) {
        g_nfc.mutex = xSemaphoreCreateMutex();
    }
    deadline_us = host_now_us + (int64_t)run_timeout_ms * 1000;
    return hal_nfc_ntag424_configure_sdm("TEST123", run_timeout_ms);
}

static void defaults(void)
{
    static const uint8_t a[1][8] = { UID_A };
    tags(1, a);
    fs_answer_len = 0;
    settings_fails_left = 0;
    production_uid = NULL;
    no_ndef_tag = -1;
    run_timeout_ms = 30000;
    const size_t lens[3] = { 7, 7, 14 };
    const uint8_t sw2[3] = { 0xAF, 0xAF, 0x00 };
    memcpy(version_len, lens, sizeof lens);
    memcpy(version_sw2, sw2, sizeof sw2);
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
    defaults();
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
        const char *name;
        uint8_t answer[24];
        size_t len;
        hal_nfc_err_t want;
    } profiles[] = {
        {
            "a file 02 that anyone may change is refused before WriteData",
            { 0x00, 0x00, 0xEE, 0xEE, 0x00, 0x01, 0x00 }, 7, HAL_NFC_ERR_NOT_SUPPORTED
        },
        {
            "a file 02 in full communication mode is refused before WriteData",
            { 0x00, 0x03, 0x00, 0xE0, 0x00, 0x01, 0x00 }, 7, HAL_NFC_ERR_NOT_SUPPORTED
        },
        {
            "a file 02 in MAC communication mode is refused before WriteData",
            { 0x00, 0x01, 0x00, 0xE0, 0x00, 0x01, 0x00 }, 7, HAL_NFC_ERR_NOT_SUPPORTED
        },
    };
    for (size_t i = 0; i < sizeof profiles / sizeof profiles[0]; i++) {
        defaults();
        file_settings(profiles[i].answer, profiles[i].len);
        r = run(1, 0, 0, 0, ACCESS_CHANGE_KEY_0);
        snprintf(d, sizeof d, "result=%d writes=%d settings=%d", r, writes, settings);
        check(profiles[i].name, writes == 0 && settings == 0 && r == profiles[i].want, d);
    }

    defaults();
    const uint8_t configured[] = { 0x00, 0x40, 0x00, 0xE0, 0x00, 0x01, 0x00, 0xC1, 0xFF, 0x22,
                                   0x20, 0x00, 0x00, 0x46, 0x00, 0x00, 0x46, 0x00, 0x00
                                 };
    file_settings(configured, sizeof configured);
    r = run(1, 0, 0, 0, ACCESS_CHANGE_KEY_0);
    hex_text(settings_block, SETTINGS_LEN, got);
    snprintf(d, sizeof d, "result=%d writes=%d settings=%s", r, writes, got);
    check("a tag this terminal configured before gets the new URL and offsets",
          r == HAL_NFC_OK && writes == 1 && memcmp(settings_block, EXPECTED_SETTINGS, SETTINGS_LEN) == 0, d);

    static const uint8_t a_then_b[2][8] = { UID_A, UID_B };
    defaults();
    tags(2, a_then_b);
    run_timeout_ms = 120000;
    r = run(1, 0, 1, 1, ACCESS_CHANGE_KEY_0);
    snprintf(d, sizeof d, "result=%d detects=%d, writes to A=%d, to B=%d", r, detect_calls, writes_to[0], writes_to[1]);
    check("a different tag found after a PN532 re-init gets no write",
          detect_calls >= 2 && writes_to[1] == 0 && r == HAL_NFC_ERR_COMM, d);

    defaults();
    tags(2, a_then_b);
    settings_fails_left = 1;
    run_timeout_ms = 120000;
    r = run(1, 0, 0, 0, ACCESS_CHANGE_KEY_0);
    snprintf(d, sizeof d, "result=%d detects=%d, writes to A=%d, to B=%d", r, detect_calls, writes_to[0], writes_to[1]);
    check("a different tag found after a write to the first one failed halfway gets no write",
          detect_calls >= 2 && writes_to[0] == 1 && writes_to[1] == 0 && r == HAL_NFC_ERR_COMM, d);

    static const uint8_t a_b_a[3][8] = { UID_A, UID_B, UID_A };
    defaults();
    tags(3, a_b_a);
    no_ndef_tag = 1;
    settings_fails_left = 1;
    run_timeout_ms = 120000;
    r = run(1, 0, 0, 0, ACCESS_CHANGE_KEY_0);
    snprintf(d, sizeof d, "result=%d detects=%d, writes to A=%d+%d, to B=%d", r, detect_calls, writes_to[0],
             writes_to[2], writes_to[1]);
    check("a write that a tag without the NDEF application interrupted finishes when the first tag returns",
          r == HAL_NFC_OK && writes_to[0] == 1 && writes_to[1] == 0 && writes_to[2] == 1, d);

    static const struct {
        const char *name;
        uint8_t uid[8];
    } uids[] = {
        { "a tag with a 4-byte random UID is refused before WriteData", { 4, 0x08, 0x12, 0x34, 0x56 } },
        { "a tag that reports no UID is refused before WriteData", { 0 } },
        { "a tag whose UID is all zeros is refused before WriteData", { 7 } },
    };
    for (size_t i = 0; i < sizeof uids / sizeof uids[0]; i++) {
        defaults();
        tags(1, &uids[i].uid);
        r = run(1, 0, 0, 0, ACCESS_CHANGE_KEY_0);
        snprintf(d, sizeof d, "result=%d writes=%d", r, writes);
        check(uids[i].name, writes == 0 && r == HAL_NFC_ERR_NOT_SUPPORTED, d);
    }

    static const struct {
        const char *name;
        size_t len[3];
        uint8_t sw2[3];
    } versions[] = {
        { "a GetVersion hardware part of 6 bytes is refused before WriteData", { 6, 7, 14 }, { 0xAF, 0xAF, 0x00 } },
        { "a GetVersion hardware part of 8 bytes is refused before WriteData", { 8, 7, 14 }, { 0xAF, 0xAF, 0x00 } },
        { "a GetVersion software part of 6 bytes is refused before WriteData", { 7, 6, 14 }, { 0xAF, 0xAF, 0x00 } },
        { "a GetVersion software part of 8 bytes is refused before WriteData", { 7, 8, 14 }, { 0xAF, 0xAF, 0x00 } },
        { "a GetVersion production part of 13 bytes is refused before WriteData", { 7, 7, 13 }, { 0xAF, 0xAF, 0x00 } },
        { "a GetVersion production part of 15 bytes is refused before WriteData", { 7, 7, 15 }, { 0xAF, 0xAF, 0x00 } },
        { "a GetVersion that ends after the hardware part is refused before WriteData", { 7, 7, 14 }, { 0x00, 0xAF, 0x00 } },
        { "a GetVersion that ends after the software part is refused before WriteData", { 7, 7, 14 }, { 0xAF, 0x00, 0x00 } },
        { "a GetVersion that goes on after the production part is refused before WriteData", { 7, 7, 14 }, { 0xAF, 0xAF, 0xAF } },
    };
    for (size_t i = 0; i < sizeof versions / sizeof versions[0]; i++) {
        defaults();
        memcpy(version_len, versions[i].len, sizeof version_len);
        memcpy(version_sw2, versions[i].sw2, sizeof version_sw2);
        run_timeout_ms = 120000;
        r = run(1, 0, 0, 0, ACCESS_CHANGE_KEY_0);
        snprintf(d, sizeof d, "result=%d writes=%d", r, writes);
        check(versions[i].name, writes == 0 && r == HAL_NFC_ERR_COMM, d);
    }

    defaults();
    static const uint8_t other_uid[8] = UID_B;
    production_uid = &other_uid[1];
    run_timeout_ms = 120000;
    r = run(1, 0, 0, 0, ACCESS_CHANGE_KEY_0);
    snprintf(d, sizeof d, "result=%d writes=%d", r, writes);
    check("a tag whose GetVersion reports another UID than its activation is refused before WriteData",
          writes == 0 && r == HAL_NFC_ERR_COMM, d);
    defaults();

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
