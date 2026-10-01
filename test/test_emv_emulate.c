// Tier 1 host unit tests for the EMV application-layer replay core
// (universal_card_reader/emv_emulate.c). Plain gcc, no Flipper SDK -- see
// ../CLAUDE.md "Two-tier testing strategy".
//
// Known-answer vectors are derived independently of the code under test: the
// SELECT PPSE command bytes below were re-derived from the ASCII name
// "2PAY.SYS.DDF01" plus ISO 7816-4 short-form case 4 framing, and the captured
// response blobs were assembled with a separate BER-TLV encoder that computes
// every length field itself. The re-derived PPSE APDU matches emv.c's own
// k_ppse_apdu byte for byte, which is the cross-check the repo rule asks for.

#include "emv_emulate.h"
#include "framework/test_framework.h"

#define AID_LEN 7

// Visa Debit/Credit AID: A0 00 00 00 03 10 10.
static const uint8_t k_aid[AID_LEN] = {
    0xa0, 0x00, 0x00, 0x00, 0x03, 0x10, 0x10,
};

// 00 A4 04 00 0E "2PAY.SYS.DDF01" 00 -- independently derived (see header).
static const uint8_t k_select_ppse[20] = {
    0x00, 0xa4, 0x04, 0x00, 0x0e, 0x32, 0x50, 0x41, 0x59, 0x2e,
    0x53, 0x59, 0x53, 0x2e, 0x44, 0x44, 0x46, 0x30, 0x31, 0x00,
};

// 00 A4 04 00 07 <AID> 00
static const uint8_t k_select_aid[13] = {
    0x00, 0xa4, 0x04, 0x00, 0x07, 0xa0, 0x00, 0x00, 0x00, 0x03, 0x10, 0x10, 0x00,
};

// 6F { 84 "2PAY.SYS.DDF01", A5 { BF0C { 61 { 4F AID, 50 "VISA DEBIT", 87 01 } } } } 9000
static const uint8_t k_ppse_blob[51] = {
    0x6f, 0x2f, 0x84, 0x0e, 0x32, 0x50, 0x41, 0x59, 0x2e, 0x53, 0x59, 0x53,
    0x2e, 0x44, 0x44, 0x46, 0x30, 0x31, 0xa5, 0x1d, 0xbf, 0x0c, 0x1a, 0x61,
    0x18, 0x4f, 0x07, 0xa0, 0x00, 0x00, 0x00, 0x03, 0x10, 0x10, 0x50, 0x0a,
    0x56, 0x49, 0x53, 0x41, 0x20, 0x44, 0x45, 0x42, 0x49, 0x54, 0x87, 0x01,
    0x01, 0x90, 0x00,
};

// 6F { 84 AID, A5 { 50 "VISA DEBIT", 9F38 PDOL, 5F2D "en" } } 9000
static const uint8_t k_adf_blob[59] = {
    0x6f, 0x37, 0x84, 0x07, 0xa0, 0x00, 0x00, 0x00, 0x03, 0x10, 0x10, 0xa5,
    0x2c, 0x50, 0x0a, 0x56, 0x49, 0x53, 0x41, 0x20, 0x44, 0x45, 0x42, 0x49,
    0x54, 0x9f, 0x38, 0x18, 0x9f, 0x66, 0x04, 0x9f, 0x02, 0x06, 0x9f, 0x03,
    0x06, 0x9f, 0x1a, 0x02, 0x95, 0x05, 0x5f, 0x2a, 0x02, 0x9a, 0x03, 0x9c,
    0x01, 0x9f, 0x37, 0x04, 0x5f, 0x2d, 0x02, 0x65, 0x6e, 0x90, 0x00,
};

// 80 { AIP 1D00, AFL 10 02 03 00 | 18 01 01 00 } 9000
static const uint8_t k_gpo_blob[14] = {
    0x80, 0x0a, 0x1d, 0x00, 0x10, 0x02, 0x03, 0x00, 0x18, 0x01, 0x01, 0x00,
    0x90, 0x00,
};

// 70 { 5A PAN, 5F24 271231, 5F30 0201 } 9000 -- AFL group 1 is SFI 2, records 2..3.
static const uint8_t k_rec_blob[25] = {
    0x70, 0x15, 0x5a, 0x08, 0x47, 0x61, 0x34, 0x00, 0x00, 0x00, 0x00, 0x18,
    0x5f, 0x24, 0x03, 0x27, 0x12, 0x31, 0x5f, 0x30, 0x02, 0x02, 0x01, 0x90,
    0x00,
};

// 80 AE 80 00 0A <10 PDOL bytes> 00 -- GENERATE AC asking for an ARQC.
static const uint8_t k_generate_ac[16] = {
    0x80, 0xae, 0x80, 0x00, 0x0a, 0x9f, 0x66, 0x04, 0x9f, 0x02,
    0x06, 0x9f, 0x03, 0x06, 0x9f, 0x00,
};

// Static: EmvReplay is ~7 KB, and these tests run one case at a time.
static EmvReplay g_fx;
static uint8_t g_out[EMV_REPLAY_MAX_LEN + 16];
static size_t g_out_len;

static void fixture_reset(void) {
    memset(&g_fx, 0, sizeof(g_fx));
    memcpy(g_fx.ppse, k_ppse_blob, sizeof(k_ppse_blob));
    g_fx.ppse_len = (uint16_t)sizeof(k_ppse_blob);
    memcpy(g_fx.adf, k_adf_blob, sizeof(k_adf_blob));
    g_fx.adf_len = (uint16_t)sizeof(k_adf_blob);
    memcpy(g_fx.adf_aid, k_aid, sizeof(k_aid));
    g_fx.adf_aid_len = (uint8_t)sizeof(k_aid);
    memcpy(g_fx.gpo, k_gpo_blob, sizeof(k_gpo_blob));
    g_fx.gpo_len = (uint16_t)sizeof(k_gpo_blob);
    g_fx.rec[0].sfi = 2;
    g_fx.rec[0].num = 2;
    memcpy(g_fx.rec[0].data, k_rec_blob, sizeof(k_rec_blob));
    g_fx.rec[0].len = (uint16_t)sizeof(k_rec_blob);
    g_fx.rec_count = 1;
}

static bool run(const uint8_t* inf, size_t inf_len) {
    g_out_len = 0;
    memset(g_out, 0xa5, sizeof(g_out));
    return emv_emu_apdu(&g_fx, inf, inf_len, g_out, sizeof(g_out), &g_out_len);
}

static void assert_sw(uint16_t sw) {
    const uint8_t want[2] = {(uint8_t)(sw >> 8), (uint8_t)(sw & 0xFF)};
    ASSERT_TRUE(g_out_len == 2);
    if(g_out_len == 2) ASSERT_BYTES_EQ(want, g_out, 2);
}

TEST_CASE(test_select_ppse_replays_capture) {
    fixture_reset();
    ASSERT_TRUE(run(k_select_ppse, sizeof(k_select_ppse)));
    ASSERT_TRUE(g_out_len == sizeof(k_ppse_blob));
    ASSERT_BYTES_EQ(k_ppse_blob, g_out, sizeof(k_ppse_blob));
}

TEST_CASE(test_select_aid_full_and_truncated) {
    fixture_reset();
    ASSERT_TRUE(run(k_select_aid, sizeof(k_select_aid)));
    ASSERT_TRUE(g_out_len == sizeof(k_adf_blob));
    ASSERT_BYTES_EQ(k_adf_blob, g_out, sizeof(k_adf_blob));

    // EMV right-truncated AID: the 5-byte prefix selects the same application.
    static const uint8_t prefix[11] = {
        0x00, 0xa4, 0x04, 0x00, 0x05, 0xa0, 0x00, 0x00, 0x00, 0x03, 0x00,
    };
    ASSERT_TRUE(run(prefix, sizeof(prefix)));
    ASSERT_TRUE(g_out_len == sizeof(k_adf_blob));
    ASSERT_BYTES_EQ(k_adf_blob, g_out, sizeof(k_adf_blob));
}

TEST_CASE(test_select_wrong_aid_is_6a82) {
    fixture_reset();
    // Mastercard AID A0 00 00 00 04 10 10 -- same length, different RID.
    static const uint8_t other[13] = {
        0x00, 0xa4, 0x04, 0x00, 0x07, 0xa0, 0x00, 0x00, 0x00, 0x04, 0x10, 0x10, 0x00,
    };
    ASSERT_TRUE(run(other, sizeof(other)));
    assert_sw(0x6a82);

    // A 4-byte "AID" is below the EMV minimum and must not match either.
    static const uint8_t too_short[10] = {
        0x00, 0xa4, 0x04, 0x00, 0x04, 0xa0, 0x00, 0x00, 0x00, 0x00,
    };
    ASSERT_TRUE(run(too_short, sizeof(too_short)));
    assert_sw(0x6a82);
}

TEST_CASE(test_select_ppse_not_captured_is_6a82) {
    fixture_reset();
    g_fx.ppse_len = 0;
    ASSERT_TRUE(run(k_select_ppse, sizeof(k_select_ppse)));
    assert_sw(0x6a82);
}

TEST_CASE(test_select_by_non_name_is_6a86) {
    fixture_reset();
    static const uint8_t by_df_name[20] = {
        0x00, 0xa4, 0x00, 0x00, 0x0e, 0x32, 0x50, 0x41, 0x59, 0x2e,
        0x53, 0x59, 0x53, 0x2e, 0x44, 0x44, 0x46, 0x30, 0x31, 0x00,
    };
    ASSERT_TRUE(run(by_df_name, sizeof(by_df_name)));
    assert_sw(0x6a86);
}

TEST_CASE(test_gpo_replays_capture) {
    fixture_reset();
    static const uint8_t gpo[8] = {0x80, 0xa8, 0x00, 0x00, 0x02, 0x83, 0x00, 0x00};
    ASSERT_TRUE(run(gpo, sizeof(gpo)));
    ASSERT_TRUE(g_out_len == sizeof(k_gpo_blob));
    ASSERT_BYTES_EQ(k_gpo_blob, g_out, sizeof(k_gpo_blob));
}

TEST_CASE(test_read_record_lookup) {
    fixture_reset();
    // 00 B2 02 14 00 -- record 2 of SFI 2, exactly what the AFL named.
    static const uint8_t hit[5] = {0x00, 0xb2, 0x02, 0x14, 0x00};
    ASSERT_TRUE(run(hit, sizeof(hit)));
    ASSERT_TRUE(g_out_len == sizeof(k_rec_blob));
    ASSERT_BYTES_EQ(k_rec_blob, g_out, sizeof(k_rec_blob));

    static const uint8_t wrong_rec[5] = {0x00, 0xb2, 0x03, 0x14, 0x00};
    ASSERT_TRUE(run(wrong_rec, sizeof(wrong_rec)));
    assert_sw(0x6a83);

    static const uint8_t wrong_sfi[5] = {0x00, 0xb2, 0x02, 0x1c, 0x00};
    ASSERT_TRUE(run(wrong_sfi, sizeof(wrong_sfi)));
    assert_sw(0x6a83);

    // Record number 0 ("first or only") was never captured by SFI+number.
    static const uint8_t zero_rec[5] = {0x00, 0xb2, 0x00, 0x14, 0x00};
    ASSERT_TRUE(run(zero_rec, sizeof(zero_rec)));
    assert_sw(0x6a83);
}

TEST_CASE(test_generate_ac_never_yields_a_cryptogram) {
    fixture_reset();
    ASSERT_TRUE(run(k_generate_ac, sizeof(k_generate_ac)));
    // Exactly two bytes: no ARQC, no CID, nothing an issuer key could have made.
    assert_sw(0x6985);
}

TEST_CASE(test_get_data_and_unknown_ins) {
    fixture_reset();
    static const uint8_t get_data[5] = {0x80, 0xca, 0x9f, 0x4f, 0x00};
    ASSERT_TRUE(run(get_data, sizeof(get_data)));
    assert_sw(0x6a88);

    static const uint8_t unknown[4] = {0x00, 0x1e, 0x00, 0x00};
    ASSERT_TRUE(run(unknown, sizeof(unknown)));
    assert_sw(0x6d00);
}

TEST_CASE(test_rejects_unparseable_and_foreign_frames) {
    fixture_reset();
    // Official firmware hands us PCB+INF: CLA guard must reject, not misanswer.
    static const uint8_t pcb_prefixed[] = {
        0x02, 0x00, 0xa4, 0x04, 0x00, 0x0e, 0x32, 0x50, 0x41, 0x59,
    };
    ASSERT_TRUE(!run(pcb_prefixed, sizeof(pcb_prefixed)));

    static const uint8_t too_short[3] = {0x00, 0xa4, 0x04};
    ASSERT_TRUE(!run(too_short, sizeof(too_short)));

    // Extended Lc (00 00 0E) is unsupported: short form only.
    static const uint8_t extended_lc[] = {
        0x00, 0xa4, 0x04, 0x00, 0x00, 0x00, 0x0e, 0x32, 0x50,
    };
    ASSERT_TRUE(!run(extended_lc, sizeof(extended_lc)));

    // Lc claims 14 bytes, only 5 follow.
    static const uint8_t truncated[10] = {
        0x00, 0xa4, 0x04, 0x00, 0x0e, 0xa0, 0x00, 0x00, 0x00, 0x03,
    };
    ASSERT_TRUE(!run(truncated, sizeof(truncated)));

    // Two trailing bytes where at most one (Le) is legal.
    static const uint8_t bad_tail[13] = {
        0x00, 0xa4, 0x04, 0x00, 0x05, 0xa0, 0x00, 0x00, 0x00, 0x03, 0x00, 0xaa, 0xbb,
    };
    ASSERT_TRUE(!run(bad_tail, sizeof(bad_tail)));
}

TEST_CASE(test_oversized_blob_emits_nothing) {
    fixture_reset();
    g_out_len = 0;
    // A blob that cannot fit must not be truncated onto the air.
    ASSERT_TRUE(!emv_emu_apdu(
        &g_fx, k_select_ppse, sizeof(k_select_ppse), g_out, 10, &g_out_len));
}

TEST_CASE(test_pcb_framing) {
    ASSERT_TRUE(emv_emu_pcb(0, false) == 0x02);
    ASSERT_TRUE(emv_emu_pcb(1, false) == 0x03);
    ASSERT_TRUE(emv_emu_pcb(0, true) == 0x12);
    ASSERT_TRUE(emv_emu_pcb(1, true) == 0x13);
    ASSERT_TRUE(emv_emu_pcb(2, false) == 0x02); // only bit 0 is the block number
    ASSERT_TRUE(emv_emu_pcb(0xff, true) == 0x13);
}

TEST_CASE(test_fsc_from_t0) {
    // FSCI is T(0)'s low nibble; the TA1/TB1/TC1 presence bits (0x10/0x20/0x40)
    // must not disturb it.
    ASSERT_TRUE(emv_emu_fsc(0x00) == 16);
    ASSERT_TRUE(emv_emu_fsc(0x02) == 32);
    ASSERT_TRUE(emv_emu_fsc(0x04) == 48);
    ASSERT_TRUE(emv_emu_fsc(0x05) == 64);
    ASSERT_TRUE(emv_emu_fsc(0x06) == 96);
    ASSERT_TRUE(emv_emu_fsc(0x07) == 128);
    ASSERT_TRUE(emv_emu_fsc(0x08) == 256);
    ASSERT_TRUE(emv_emu_fsc(0x09) == 512);
    ASSERT_TRUE(emv_emu_fsc(0x28) == 256); // TB(1) present, FSCI 8
    ASSERT_TRUE(emv_emu_fsc(0x15) == 64); // TA(1) present, FSCI 5
    ASSERT_TRUE(emv_emu_fsc(0x48) == 256); // TC(1) present, FSCI 8
    ASSERT_TRUE(emv_emu_fsc(0x0d) == 32); // RFU FSCI -> default
    ASSERT_TRUE(emv_emu_fsc(0x7f) == 32); // RFU FSCI -> default
}

TEST_CASE(test_chunk_max) {
    ASSERT_TRUE(emv_emu_chunk_max(64) == 61);
    ASSERT_TRUE(emv_emu_chunk_max(16) == 13);
    ASSERT_TRUE(emv_emu_chunk_max(256) == 253);
    ASSERT_TRUE(emv_emu_chunk_max(3) == 0); // no room for even one INF byte
    ASSERT_TRUE(emv_emu_chunk_max(0) == 0);
}

TEST_CASE(test_mutation_is_caught) {
    // Proves the byte-for-byte comparisons above are real: corrupting one
    // captured byte must change what the terminal would receive.
    uint8_t good[EMV_REPLAY_MAX_LEN + 16];
    size_t good_len = 0;

    fixture_reset();
    ASSERT_TRUE(emv_emu_apdu(
        &g_fx, k_select_ppse, sizeof(k_select_ppse), good, sizeof(good), &good_len));

    g_fx.ppse[20] ^= 0x01;
    ASSERT_TRUE(run(k_select_ppse, sizeof(k_select_ppse)));
    ASSERT_TRUE(g_out_len == good_len);
    ASSERT_TRUE(good_len == sizeof(k_ppse_blob) && memcmp(good, g_out, good_len) != 0);
}

int main(void) {
    RUN_TEST(test_select_ppse_replays_capture);
    RUN_TEST(test_select_aid_full_and_truncated);
    RUN_TEST(test_select_wrong_aid_is_6a82);
    RUN_TEST(test_select_ppse_not_captured_is_6a82);
    RUN_TEST(test_select_by_non_name_is_6a86);
    RUN_TEST(test_gpo_replays_capture);
    RUN_TEST(test_read_record_lookup);
    RUN_TEST(test_generate_ac_never_yields_a_cryptogram);
    RUN_TEST(test_get_data_and_unknown_ins);
    RUN_TEST(test_rejects_unparseable_and_foreign_frames);
    RUN_TEST(test_oversized_blob_emits_nothing);
    RUN_TEST(test_pcb_framing);
    RUN_TEST(test_fsc_from_t0);
    RUN_TEST(test_chunk_max);
    RUN_TEST(test_mutation_is_caught);
    return test_report();
}
