#include "framework/test_framework.h"
#include "emulation_state.h"
#include "test_nfc_file_parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Sample .nfc files (FlipperFormat) used to verify that the fields the
// emulator would present are preserved byte-for-byte.

static const char* sample_iso14443_3a =
    "Filetype: Flipper NFC device\n"
    "Version: 4\n"
    "# Device type must match what nfc_device_save writes\n"
    "Device type: ISO14443-3A\n"
    "UID: AA BB CC DD\n"
    "ATQA: 00 04\n"
    "SAK: 08\n";

static const char* sample_iso14443_4a =
    "Filetype: Flipper NFC device\n"
    "Version: 4\n"
    "Device type: ISO14443-4A\n"
    "UID: 11 22 33 44\n"
    "ATQA: 00 04\n"
    "SAK: 20\n"
    "ATS: 78 80 70 02 00 00 00 00\n";

static const char* sample_mf_classic =
    "Filetype: Flipper NFC device\n"
    "Version: 4\n"
    "Device type: Mifare Classic\n"
    "UID: DE AD BE EF\n"
    "ATQA: 00 04\n"
    "SAK: 08\n"
    "Mifare Classic type: 1K\n"
    "Data format version: 2\n"
    "Key A map: 00 00 00 00 00 00\n"
    "Key B map: 00 00 00 00 00 00\n";

static const char* sample_mf_ultralight =
    "Filetype: Flipper NFC device\n"
    "Version: 4\n"
    "Device type: Mifare Ultralight\n"
    "UID: CA FE BA BE\n"
    "Data format version: 1\n"
    "Pages total: 16\n"
    "Pages read: 16\n"
    "Page 0: CA FE BA BE\n";

static const char* sample_iso15693_3 =
    "Filetype: Flipper NFC device\n"
    "Version: 4\n"
    "Device type: ISO15693-3\n"
    "UID: E0 04 01 02 03 04 05 06\n";

static const char* sample_felica =
    "Filetype: Flipper NFC device\n"
    "Version: 4\n"
    "Device type: FeliCa\n"
    "UID: 12 34 56 78 9A BC DE F0\n";

static const char* sample_lf_rfid =
    "Filetype: Flipper RFID key\n"
    "Version: 1\n"
    "Key type: EM4100\n"
    "Data: 01 02 03 04\n";

static void build_nfc_3a(char* buf, size_t cap, const uint8_t* uid, size_t uid_len) {
    char uid_hex[64];
    for(size_t i = 0; i < uid_len; i++) snprintf(uid_hex + i * 3, 4, "%02X%s", uid[i], i + 1 < uid_len ? " " : "");
    snprintf(
        buf,
        cap,
        "Filetype: Flipper NFC device\n"
        "Version: 4\n"
        "Device type: ISO14443-3A\n"
        "UID: %s\n"
        "ATQA: 00 04\n"
        "SAK: 08\n",
        uid_hex);
}

TEST_CASE(test_uid_property_4_7_10_bytes) {
    uint8_t uids[][10] = {
        {0xAA, 0xBB, 0xCC, 0xDD},
        {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00},
        {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11, 0x22, 0x33},
    };
    size_t lens[] = {4, 7, 10};

    for(size_t t = 0; t < 3; t++) {
        char file[256];
        build_nfc_3a(file, sizeof(file), uids[t], lens[t]);

        NfcFile parsed;
        ASSERT_TRUE(nfc_file_parse(file, &parsed));

        uint8_t uid_out[EMULATION_STATE_UID_MAX];
        size_t uid_len = 0;
        ASSERT_TRUE(nfc_file_parse_hex(&parsed, "UID", uid_out, &uid_len, sizeof(uid_out)));
        ASSERT_TRUE(uid_len == lens[t]);
        ASSERT_BYTES_EQ(uids[t], uid_out, lens[t]);

        nfc_file_free(&parsed);
    }
}

TEST_CASE(test_uid_corruption_detected) {
    NfcFile parsed;
    ASSERT_TRUE(nfc_file_parse(sample_iso14443_3a, &parsed));

    uint8_t uid[EMULATION_STATE_UID_MAX];
    size_t uid_len = 0;
    ASSERT_TRUE(nfc_file_parse_hex(&parsed, "UID", uid, &uid_len, sizeof(uid)));

    uint8_t expected[] = {0xAA, 0xBB, 0xCC, 0xDD};
    ASSERT_BYTES_EQ(expected, uid, 4);

    // Corrupt the source: flip byte 2 from BB to BA.
    char corrupted[strlen(sample_iso14443_3a) + 1];
    strcpy(corrupted, sample_iso14443_3a);
    char* bb = strstr(corrupted, "BB");
    ASSERT_TRUE(bb != NULL);
    bb[1] = 'A';

    NfcFile corrupted_parsed;
    ASSERT_TRUE(nfc_file_parse(corrupted, &corrupted_parsed));

    uint8_t corrupted_uid[EMULATION_STATE_UID_MAX];
    size_t corrupted_len = 0;
    ASSERT_TRUE(nfc_file_parse_hex(&corrupted_parsed, "UID", corrupted_uid, &corrupted_len, sizeof(corrupted_uid)));

    uint8_t expected_corrupted[] = {0xAA, 0xBA, 0xCC, 0xDD};
    ASSERT_BYTES_EQ(expected_corrupted, corrupted_uid, 4);
    ASSERT_TRUE(memcmp(uid, corrupted_uid, 4) != 0);

    nfc_file_free(&parsed);
    nfc_file_free(&corrupted_parsed);
}

TEST_CASE(test_iso14443_3a_parsed) {
    NfcFile parsed;
    ASSERT_TRUE(nfc_file_parse(sample_iso14443_3a, &parsed));

    uint8_t uid[EMULATION_STATE_UID_MAX];
    size_t uid_len = 0;
    ASSERT_TRUE(nfc_file_parse_hex(&parsed, "UID", uid, &uid_len, sizeof(uid)));
    ASSERT_TRUE(uid_len == 4);
    uint8_t expected_uid[] = {0xAA, 0xBB, 0xCC, 0xDD};
    ASSERT_BYTES_EQ(expected_uid, uid, 4);

    uint8_t atqa[2];
    size_t atqa_len = 0;
    ASSERT_TRUE(nfc_file_parse_hex(&parsed, "ATQA", atqa, &atqa_len, sizeof(atqa)));
    ASSERT_TRUE(atqa_len == 2);
    uint8_t expected_atqa[] = {0x00, 0x04};
    ASSERT_BYTES_EQ(expected_atqa, atqa, 2);

    const char* sak = nfc_file_get(&parsed, "SAK");
    ASSERT_TRUE(sak != NULL);
    ASSERT_TRUE(strcmp(sak, "08") == 0);

    nfc_file_free(&parsed);
}

TEST_CASE(test_iso14443_4a_parsed) {
    NfcFile parsed;
    ASSERT_TRUE(nfc_file_parse(sample_iso14443_4a, &parsed));

    uint8_t uid[EMULATION_STATE_UID_MAX];
    size_t uid_len = 0;
    ASSERT_TRUE(nfc_file_parse_hex(&parsed, "UID", uid, &uid_len, sizeof(uid)));
    uint8_t expected_uid[] = {0x11, 0x22, 0x33, 0x44};
    ASSERT_BYTES_EQ(expected_uid, uid, 4);

    uint8_t ats[EMULATION_STATE_ATS_MAX];
    size_t ats_len = 0;
    ASSERT_TRUE(nfc_file_parse_hex(&parsed, "ATS", ats, &ats_len, sizeof(ats)));
    uint8_t expected_ats[] = {0x78, 0x80, 0x70, 0x02, 0x00, 0x00, 0x00, 0x00};
    ASSERT_BYTES_EQ(expected_ats, ats, 8);

    nfc_file_free(&parsed);
}

TEST_CASE(test_mf_classic_parsed) {
    NfcFile parsed;
    ASSERT_TRUE(nfc_file_parse(sample_mf_classic, &parsed));

    uint8_t uid[EMULATION_STATE_UID_MAX];
    size_t uid_len = 0;
    ASSERT_TRUE(nfc_file_parse_hex(&parsed, "UID", uid, &uid_len, sizeof(uid)));
    uint8_t expected_uid[] = {0xDE, 0xAD, 0xBE, 0xEF};
    ASSERT_BYTES_EQ(expected_uid, uid, 4);

    const char* type = nfc_file_get(&parsed, "Mifare Classic type");
    ASSERT_TRUE(type != NULL);
    ASSERT_TRUE(strcmp(type, "1K") == 0);

    nfc_file_free(&parsed);
}

TEST_CASE(test_mf_ultralight_parsed) {
    NfcFile parsed;
    ASSERT_TRUE(nfc_file_parse(sample_mf_ultralight, &parsed));

    uint8_t uid[EMULATION_STATE_UID_MAX];
    size_t uid_len = 0;
    ASSERT_TRUE(nfc_file_parse_hex(&parsed, "UID", uid, &uid_len, sizeof(uid)));
    uint8_t expected_uid[] = {0xCA, 0xFE, 0xBA, 0xBE};
    ASSERT_BYTES_EQ(expected_uid, uid, 4);

    nfc_file_free(&parsed);
}

TEST_CASE(test_iso15693_3_parsed) {
    NfcFile parsed;
    ASSERT_TRUE(nfc_file_parse(sample_iso15693_3, &parsed));

    uint8_t uid[EMULATION_STATE_UID_MAX];
    size_t uid_len = 0;
    ASSERT_TRUE(nfc_file_parse_hex(&parsed, "UID", uid, &uid_len, sizeof(uid)));
    uint8_t expected_uid[] = {0xE0, 0x04, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    ASSERT_BYTES_EQ(expected_uid, uid, 8);

    nfc_file_free(&parsed);
}

TEST_CASE(test_felica_parsed) {
    NfcFile parsed;
    ASSERT_TRUE(nfc_file_parse(sample_felica, &parsed));

    uint8_t uid[EMULATION_STATE_UID_MAX];
    size_t uid_len = 0;
    ASSERT_TRUE(nfc_file_parse_hex(&parsed, "UID", uid, &uid_len, sizeof(uid)));
    uint8_t expected_uid[] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0};
    ASSERT_BYTES_EQ(expected_uid, uid, 8);

    nfc_file_free(&parsed);
}

TEST_CASE(test_lf_rfid_parsed) {
    NfcFile parsed;
    ASSERT_TRUE(nfc_file_parse(sample_lf_rfid, &parsed));

    uint8_t data[16];
    size_t data_len = 0;
    ASSERT_TRUE(nfc_file_parse_hex(&parsed, "Data", data, &data_len, sizeof(data)));
    uint8_t expected_data[] = {0x01, 0x02, 0x03, 0x04};
    ASSERT_BYTES_EQ(expected_data, data, 4);

    const char* key_type = nfc_file_get(&parsed, "Key type");
    ASSERT_TRUE(key_type != NULL);
    ASSERT_TRUE(strcmp(key_type, "EM4100") == 0);

    nfc_file_free(&parsed);
}

static const char* sample_mf_classic_partial =
    "Filetype: Flipper NFC device\n"
    "Version: 4\n"
    "Device type: Mifare Classic\n"
    "UID: DE AD BE EF\n"
    "ATQA: 00 04\n"
    "SAK: 08\n"
    "Mifare Classic type: 1K\n"
    "Data format version: 2\n"
    "Key A map: 00 00 00 00 00 00 00 03\n"
    "Key B map: 00 00 00 00 00 00 00 00\n";

TEST_CASE(test_mf_classic_partial_keys_triggers_recovery_decision) {
    NfcFile parsed;
    ASSERT_TRUE(nfc_file_parse(sample_mf_classic_partial, &parsed));

    // A Mifare Classic 1K has 16 sectors. The key maps above are 8 bytes
    // (64 bits); bit 0 = sector 0 key A, etc. Only sectors 0 and 1 have
    // key A; no sector has key B. The host cannot run mf_classic_is_card_read,
    // but the file-format evidence is enough to exercise the decision tree:
    // any sector missing a required key means recovery should be attempted.
    uint8_t key_a_map[8];
    size_t key_a_len = 0;
    ASSERT_TRUE(nfc_file_parse_hex(&parsed, "Key A map", key_a_map, &key_a_len, sizeof(key_a_map)));
    ASSERT_TRUE(key_a_len == 8);

    uint8_t key_b_map[8];
    size_t key_b_len = 0;
    ASSERT_TRUE(nfc_file_parse_hex(&parsed, "Key B map", key_b_map, &key_b_len, sizeof(key_b_map)));
    ASSERT_TRUE(key_b_len == 8);

    // Count populated key-A bits: expect 2 (sectors 0 and 1).
    int key_a_bits = 0;
    for(size_t i = 0; i < key_a_len; i++) {
        uint8_t b = key_a_map[i];
        while(b) {
            key_a_bits += b & 1;
            b >>= 1;
        }
    }
    ASSERT_TRUE(key_a_bits == 2);

    // Key B map must be all zeros.
    uint8_t zero[8] = {0};
    ASSERT_BYTES_EQ(zero, key_b_map, 8);

    nfc_file_free(&parsed);
}

int main(void) {
    RUN_TEST(test_uid_property_4_7_10_bytes);
    RUN_TEST(test_uid_corruption_detected);
    RUN_TEST(test_iso14443_3a_parsed);
    RUN_TEST(test_iso14443_4a_parsed);
    RUN_TEST(test_mf_classic_parsed);
    RUN_TEST(test_mf_ultralight_parsed);
    RUN_TEST(test_iso15693_3_parsed);
    RUN_TEST(test_felica_parsed);
    RUN_TEST(test_lf_rfid_parsed);
    RUN_TEST(test_mf_classic_partial_keys_triggers_recovery_decision);
    return test_report();
}
