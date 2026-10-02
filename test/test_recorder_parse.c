// Tier 1 host unit tests for the SubGHz recorder's live-readout frame parser
// (recorder_parse.c). Plain gcc, no Flipper SDK -- see ../CLAUDE.md "Two-tier
// testing strategy".
//
// The expected strings below are NOT copied from recorder_parse.c: they are
// derived from the firmware's own get_string() format strings, read off
// lib/subghz/protocols/*.c in official 1.4.3 --
//   princeton: "%s %dbit\r\nKey:0x%08lX\r\nYek:0x%08lX\r\nSn:0x%05lX Btn:%01X\r\nTe:%luus ..."
//   keeloq:    "Key:%08lX%08lX\r\n"
//   came/nice_flo: "Key:0x%08lX\r\n"
//   linear:    "Key:0x%03lX\r\n"
// which is exactly the spread of Key-line shapes the parser has to survive.

#include "recorder_parse.h"
#include "framework/test_framework.h"

#define PROTO_BUF 24
#define KEY_BUF   17

static void parse(const char* frame, char* proto, char* key) {
    sub_rec_live_parse(frame, proto, PROTO_BUF, key, KEY_BUF);
}

TEST_CASE(test_princeton_frame) {
    char proto[PROTO_BUF], key[KEY_BUF];
    parse(
        "Princeton 24bit\r\n"
        "Key:0x00F6D942\r\n"
        "Yek:0x42D9F600\r\n"
        "Sn:0x00F6D Btn:4\r\n"
        "Te:400us  GT:Te*36\r\n",
        proto,
        key);
    ASSERT_TRUE(strcmp(proto, "Princeton 24bit") == 0);
    // The 0x prefix is dropped; the later "Yek:" line must not be picked up.
    ASSERT_TRUE(strcmp(key, "00F6D942") == 0);
}

TEST_CASE(test_keeloq_frame_no_prefix_no_spaces) {
    char proto[PROTO_BUF], key[KEY_BUF];
    parse(
        "KeeLoq 64bit\r\n"
        "Key:0011223344556677\r\n"
        "Manufacture name: DoorHan\r\n"
        "Cnt:0003\r\n",
        proto,
        key);
    ASSERT_TRUE(strcmp(proto, "KeeLoq 64bit") == 0);
    ASSERT_TRUE(strcmp(key, "0011223344556677") == 0);
}

TEST_CASE(test_came_frame_short_key) {
    char proto[PROTO_BUF], key[KEY_BUF];
    parse("CAME 12bit\r\nKey:0x000FFF\r\n", proto, key);
    ASSERT_TRUE(strcmp(proto, "CAME 12bit") == 0);
    ASSERT_TRUE(strcmp(key, "000FFF") == 0);
}

TEST_CASE(test_lowercase_prefix_kept_verbatim) {
    char proto[PROTO_BUF], key[KEY_BUF];
    parse("CAME 24bit\r\nKey:0xabcdef\r\n", proto, key);
    // Case is the decoder's, not ours to normalise.
    ASSERT_TRUE(strcmp(key, "abcdef") == 0);
}

TEST_CASE(test_frame_without_key_line) {
    char proto[PROTO_BUF], key[KEY_BUF];
    parse("RAW 137bit\r\nSn:0000\r\n", proto, key);
    ASSERT_TRUE(strcmp(proto, "RAW 137bit") == 0);
    ASSERT_TRUE(key[0] == '\0');
}

TEST_CASE(test_frame_without_trailing_cr) {
    char proto[PROTO_BUF], key[KEY_BUF];
    parse("Linear 10bit\r\nKey:0x3FF", proto, key);
    ASSERT_TRUE(strcmp(proto, "Linear 10bit") == 0);
    ASSERT_TRUE(strcmp(key, "3FF") == 0);
}

TEST_CASE(test_spaced_bytes_are_collapsed) {
    // The .sub file format writes Key as space-separated bytes; a decoder
    // that ever adopted it must not leak the spaces into the readout.
    char proto[PROTO_BUF], key[KEY_BUF];
    parse("CAME 24bit\r\nKey:00 00 00 00 00 00 0F F1\r\n", proto, key);
    ASSERT_TRUE(strcmp(key, "0000000000000FF1") == 0);
}

TEST_CASE(test_buffers_truncate_not_overrun) {
    char proto[10], key[5];
    sub_rec_live_parse(
        "Princeton 24bit\r\nKey:0x0011223344556677\r\n", proto, sizeof(proto), key, sizeof(key));
    ASSERT_TRUE(strcmp(proto, "Princeton") == 0);
    ASSERT_TRUE(strcmp(key, "0011") == 0);
}

TEST_CASE(test_empty_and_null_frames) {
    char proto[PROTO_BUF], key[KEY_BUF];
    parse("", proto, key);
    ASSERT_TRUE(proto[0] == '\0');
    ASSERT_TRUE(key[0] == '\0');
    sub_rec_live_parse(NULL, proto, sizeof(proto), key, sizeof(key));
    ASSERT_TRUE(proto[0] == '\0');
    ASSERT_TRUE(key[0] == '\0');
}

int main(void) {
    RUN_TEST(test_princeton_frame);
    RUN_TEST(test_keeloq_frame_no_prefix_no_spaces);
    RUN_TEST(test_came_frame_short_key);
    RUN_TEST(test_lowercase_prefix_kept_verbatim);
    RUN_TEST(test_frame_without_key_line);
    RUN_TEST(test_frame_without_trailing_cr);
    RUN_TEST(test_spaced_bytes_are_collapsed);
    RUN_TEST(test_buffers_truncate_not_overrun);
    RUN_TEST(test_empty_and_null_frames);
    return test_report();
}
