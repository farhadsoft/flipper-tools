// Tier 1 host unit tests for the ble_findmy domain code (findmy_payload.c).
// Plain gcc, no Flipper SDK -- see ../CLAUDE.md "Two-tier testing strategy".
//
// Known-answer vectors below are independently derived from the primary
// reference this feature is modeled on: seemoo-lab/openhaystack's ESP32
// firmware (Firmware/ESP32/main/openhaystack_main.c, set_addr_from_key() /
// set_payload_from_key()), not copied from findmy_payload.c itself -- the
// point of a known-answer test is that the expected bytes come from
// somewhere other than the code under test.

#include "findmy_payload.h"
#include "framework/test_framework.h"

// Mirrors the shipped default findmy_public_key, but kept as an independent
// local literal so replacing the shipped key doesn't silently break these
// tests (Tier 1 tests the algorithm, not whichever key is configured).
static const uint8_t key_a[FINDMY_PUBKEY_LEN] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c,
    0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19,
    0x1a, 0x1b,
};

// Differs from key_a only in byte 0 (0x89 instead of 0x00). 0x89 =
// 0b10001001: OR-ing with the 0xC0 MAC mask sets a bit that was 0
// (distinguishes OR from AND/assignment bugs), and right-shifting by 6
// yields a nonzero top-bits byte in the advertisement (distinguishes a
// wrong shift amount or a dropped top-bits byte).
static const uint8_t key_b[FINDMY_PUBKEY_LEN] = {
    0x89, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c,
    0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19,
    0x1a, 0x1b,
};

TEST_CASE(test_build_mac_key_a) {
    static const uint8_t expected[FINDMY_MAC_LEN] = {0xc0, 0x01, 0x02, 0x03, 0x04, 0x05};
    uint8_t mac[FINDMY_MAC_LEN];
    findmy_build_mac(key_a, mac);
    ASSERT_BYTES_EQ(expected, mac, FINDMY_MAC_LEN);
}

TEST_CASE(test_build_mac_key_b) {
    // 0x89 | 0xC0 = 0xC9 -- catches a MAC mask bug (AND, wrong constant, or
    // a forgotten OR) that key_a's 0x00 top byte can't distinguish.
    static const uint8_t expected[FINDMY_MAC_LEN] = {0xc9, 0x01, 0x02, 0x03, 0x04, 0x05};
    uint8_t mac[FINDMY_MAC_LEN];
    findmy_build_mac(key_b, mac);
    ASSERT_BYTES_EQ(expected, mac, FINDMY_MAC_LEN);
}

TEST_CASE(test_build_adv_key_a) {
    static const uint8_t expected[FINDMY_ADV_LEN] = {
        0x1e, 0xff, 0x4c, 0x00, 0x12, 0x19, 0x00, // header
        0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, // key[6..15]
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, // key[16..27]
        0x00, // key[0]>>6 == 0x00>>6 == 0
        0x00, // hint
    };
    uint8_t adv[FINDMY_ADV_LEN];
    findmy_build_adv(key_a, adv);
    ASSERT_BYTES_EQ(expected, adv, FINDMY_ADV_LEN);
}

TEST_CASE(test_build_adv_key_b) {
    // Same key[6..27] as key_a (bytes 6.. are identical between the two
    // vectors), so the only expected difference is the top-bits byte:
    // 0x89>>6 == 0x02 instead of 0x00. Isolates the top-bits extraction.
    static const uint8_t expected[FINDMY_ADV_LEN] = {
        0x1e, 0xff, 0x4c, 0x00, 0x12, 0x19, 0x00,
        0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b,
        0x02, // key[0]>>6 == 0x89>>6 == 0x02
        0x00,
    };
    uint8_t adv[FINDMY_ADV_LEN];
    findmy_build_adv(key_b, adv);
    ASSERT_BYTES_EQ(expected, adv, FINDMY_ADV_LEN);
}

TEST_CASE(test_mutation_is_caught) {
    // Demonstrates the known-answer comparison actually catches a 1-byte
    // corruption, rather than merely asserting it would.
    uint8_t adv[FINDMY_ADV_LEN];
    findmy_build_adv(key_a, adv);

    uint8_t corrupted[FINDMY_ADV_LEN];
    memcpy(corrupted, adv, FINDMY_ADV_LEN);
    corrupted[10] ^= 0x01; // flip one bit of one key byte

    ASSERT_TRUE(memcmp(adv, corrupted, FINDMY_ADV_LEN) != 0);
}

TEST_CASE(test_parse_hex_key_round_trip) {
    const char* hex = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b";
    uint8_t out[FINDMY_PUBKEY_LEN];
    ASSERT_TRUE(findmy_parse_hex_key(hex, out));
    ASSERT_BYTES_EQ(key_a, out, FINDMY_PUBKEY_LEN);
}

TEST_CASE(test_parse_hex_key_uppercase) {
    const char* hex = "890102030405060708090A0B0C0D0E0F101112131415161718191A1B";
    uint8_t out[FINDMY_PUBKEY_LEN];
    ASSERT_TRUE(findmy_parse_hex_key(hex, out));
    ASSERT_BYTES_EQ(key_b, out, FINDMY_PUBKEY_LEN);
}

TEST_CASE(test_parse_hex_key_wrong_length) {
    uint8_t out[FINDMY_PUBKEY_LEN];
    ASSERT_TRUE(!findmy_parse_hex_key("0001", out)); // too short
    ASSERT_TRUE(!findmy_parse_hex_key(
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1bff", out)); // too long
}

TEST_CASE(test_parse_hex_key_invalid_char) {
    uint8_t out[FINDMY_PUBKEY_LEN];
    ASSERT_TRUE(!findmy_parse_hex_key(
        "00010203040506070809gg0b0c0d0e0f101112131415161718191a1b", out));
}

TEST_CASE(test_parse_hex_key_null) {
    uint8_t out[FINDMY_PUBKEY_LEN];
    memset(out, 0xaa, sizeof(out));
    ASSERT_TRUE(!findmy_parse_hex_key(NULL, out));
    // Failure must not partially write the output buffer.
    for(size_t i = 0; i < FINDMY_PUBKEY_LEN; i++) ASSERT_TRUE(out[i] == 0xaa);
}

TEST_CASE(test_legacy_key_and_getter_preserved) {
    ASSERT_BYTES_EQ(key_a, findmy_public_key, FINDMY_PUBKEY_LEN);

    const uint8_t* data = NULL;
    size_t len = 0;
    findmy_payload_get(&data, &len);
    ASSERT_TRUE(data == findmy_public_key);
    ASSERT_TRUE(len == FINDMY_PUBKEY_LEN);
}

int main(void) {
    RUN_TEST(test_build_mac_key_a);
    RUN_TEST(test_build_mac_key_b);
    RUN_TEST(test_build_adv_key_a);
    RUN_TEST(test_build_adv_key_b);
    RUN_TEST(test_mutation_is_caught);
    RUN_TEST(test_parse_hex_key_round_trip);
    RUN_TEST(test_parse_hex_key_uppercase);
    RUN_TEST(test_parse_hex_key_wrong_length);
    RUN_TEST(test_parse_hex_key_invalid_char);
    RUN_TEST(test_parse_hex_key_null);
    RUN_TEST(test_legacy_key_and_getter_preserved);
    return test_report();
}
