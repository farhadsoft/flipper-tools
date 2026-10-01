#pragma once

/*
 * HAL-free EMV application-layer replay core.
 * ------------------------------------------------------------
 * Pure domain code: stdint/stddef/stdbool only, no furi/SDK includes, so it
 * compiles in the Tier-1 host test harness (see ../../test/Makefile) exactly
 * like modules/ble_findmy/findmy_payload.{c,h}.
 *
 * Emulating a bank card replays the responses the real card gave during
 * emv_read(): SELECT PPSE, SELECT AID, GET PROCESSING OPTIONS and every AFL
 * READ RECORD. GENERATE AC is answered with SW 6985 ("conditions of use not
 * satisfied") and never with a fabricated cryptogram: an online EMV
 * transaction needs an ARQC computed with the issuer secret key sealed in the
 * card's secure element, which is unreadable and not reproducible here.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define EMV_REPLAY_MAX_LEN 256 // one captured APDU response (INF incl. SW)
#define EMV_REPLAY_MAX_RECORDS 24 // matches emv_read_records()'s total_read cap
#define EMV_REPLAY_AID_MAX 16 // == EMV_AID_MAX_LEN in emv.h

typedef struct {
    uint8_t sfi;
    uint8_t num;
    uint16_t len;
    uint8_t data[EMV_REPLAY_MAX_LEN];
} EmvReplayRecord;

typedef struct {
    uint16_t ppse_len; // SELECT 2PAY.SYS.DDF01 response, FCI+SW
    uint8_t ppse[EMV_REPLAY_MAX_LEN];
    uint16_t adf_len; // SELECT AID response of the selected AID, FCI+SW
    uint8_t adf[EMV_REPLAY_MAX_LEN];
    uint8_t adf_aid_len;
    uint8_t adf_aid[EMV_REPLAY_AID_MAX];
    uint16_t gpo_len; // GPO response, template+SW
    uint8_t gpo[EMV_REPLAY_MAX_LEN];
    uint8_t rec_count;
    EmvReplayRecord rec[EMV_REPLAY_MAX_RECORDS];
} EmvReplay;

/**
 * Dispatch one received INF (the bare APDU; Momentum's ISO14443-4A listener
 * strips the ISO-DEP prologue before handing it up).
 *
 * On true, out[0..*out_len) holds the response INF including its two SW
 * bytes. On false send nothing at all: the frame was unrecognized, foreign
 * (e.g. an official-firmware PCB-prefixed buffer), or too big for `out_cap`.
 */
bool emv_emu_apdu(
    const EmvReplay* r,
    const uint8_t* inf,
    size_t inf_len,
    uint8_t* out,
    size_t out_cap,
    size_t* out_len);

// I-block PCB: 0x02 is the mandatory I-block marker, the block number is
// bit 0, chaining is bit 4 (Momentum's iso14443_4_layer.c defines). No CID
// byte -- see the README limitation.
uint8_t emv_emu_pcb(uint8_t block_num, bool chain);

// Frame Size for proximity Card, from ATS byte T(0). FSCI is T(0)'s low
// nibble -- NOT TB(1), which carries FWI/SFGI. Mirrors the firmware's own
// iso14443_4a_get_frame_size_max() mapping; its RFU-FSCI 13..15 answer of 0
// becomes the ISO14443-4 default 32 here, so chunking always has a bound.
uint16_t emv_emu_fsc(uint8_t t0);

// Max INF bytes per response frame: FSC minus PCB (1) and CRC-A (2).
size_t emv_emu_chunk_max(uint16_t fsc);
