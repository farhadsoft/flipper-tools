#include "framework/test_framework.h"
#include "test_nfc_file_parse.h"

#include <stdio.h>
#include <string.h>

static const char* sample_nfc =
    "Filetype: Flipper NFC device\n"
    "Version: 4\n"
    "Device type: ISO14443-4A\n"
    "UID: 11 22 33 44\n"
    "ATQA: 00 04\n"
    "SAK: 20\n"
    "ATS: 78 80 70 02 00 00 00 00\n";

static const char* sample_rfid =
    "Filetype: Flipper RFID key\n"
    "Version: 1\n"
    "Key type: EM4100\n"
    "Data: 01 02 03 04\n";

static void assert_file_eq(const NfcFile* a, const NfcFile* b) {
    ASSERT_TRUE(a->count == b->count);
    if(a->count != b->count) return;
    for(size_t i = 0; i < a->count; i++) {
        ASSERT_TRUE(strcmp(a->entries[i].key, b->entries[i].key) == 0);
        ASSERT_TRUE(strcmp(a->entries[i].value, b->entries[i].value) == 0);
    }
}

TEST_CASE(test_nfc_roundtrip_preserved) {
    NfcFile parsed;
    ASSERT_TRUE(nfc_file_parse(sample_nfc, &parsed));

    char serialized[512];
    size_t len = 0;
    ASSERT_TRUE(nfc_file_serialize(&parsed, serialized, sizeof(serialized), &len));

    NfcFile reparsed;
    ASSERT_TRUE(nfc_file_parse(serialized, &reparsed));

    assert_file_eq(&parsed, &reparsed);

    nfc_file_free(&parsed);
    nfc_file_free(&reparsed);
}

TEST_CASE(test_rfid_roundtrip_preserved) {
    NfcFile parsed;
    ASSERT_TRUE(nfc_file_parse(sample_rfid, &parsed));

    char serialized[512];
    size_t len = 0;
    ASSERT_TRUE(nfc_file_serialize(&parsed, serialized, sizeof(serialized), &len));

    NfcFile reparsed;
    ASSERT_TRUE(nfc_file_parse(serialized, &reparsed));

    assert_file_eq(&parsed, &reparsed);

    nfc_file_free(&parsed);
    nfc_file_free(&reparsed);
}

TEST_CASE(test_set_value_roundtrip) {
    NfcFile parsed;
    ASSERT_TRUE(nfc_file_parse(sample_nfc, &parsed));

    ASSERT_TRUE(nfc_file_set(&parsed, "UID", "AA BB CC DD"));
    ASSERT_TRUE(nfc_file_set(&parsed, "New key", "new value"));

    char serialized[512];
    ASSERT_TRUE(nfc_file_serialize(&parsed, serialized, sizeof(serialized), NULL));

    NfcFile reparsed;
    ASSERT_TRUE(nfc_file_parse(serialized, &reparsed));

    ASSERT_TRUE(strcmp(nfc_file_get(&reparsed, "UID"), "AA BB CC DD") == 0);
    ASSERT_TRUE(strcmp(nfc_file_get(&reparsed, "New key"), "new value") == 0);
    ASSERT_TRUE(nfc_file_get(&reparsed, "Missing key") == NULL);

    nfc_file_free(&parsed);
    nfc_file_free(&reparsed);
}

int main(void) {
    RUN_TEST(test_nfc_roundtrip_preserved);
    RUN_TEST(test_rfid_roundtrip_preserved);
    RUN_TEST(test_set_value_roundtrip);
    return test_report();
}
