/*
 * HAL-free EMV application-layer replay core -- see emv_emulate.h.
 *
 * The only behaviour here is the one EMV terminal exchange a captured card
 * can honestly replay: SELECT (PPSE + AID), GET PROCESSING OPTIONS, READ
 * RECORD. GENERATE AC answers SW 6985 and never a cryptogram: the ARQC an
 * online transaction needs is computed with an issuer secret key sealed in the
 * card's secure element -- unreadable, uncaptured, unreproducible.
 */

#include "emv_emulate.h"

#include <string.h>

// APDU status words this file emits. Every replayed blob already carries its
// own captured SW, so these are only for answers we synthesize.
#define SW_CONDITIONS_NOT_SATISFIED 0x6985 // GENERATE AC: no cryptogram, ever
#define SW_INCORRECT_P1_P2          0x6A86 // SELECT by anything but name
#define SW_FILE_NOT_FOUND           0x6A82 // nothing captured for this SELECT/GPO
#define SW_RECORD_NOT_FOUND         0x6A83 // unknown SFI / record number
#define SW_REF_DATA_NOT_FOUND       0x6A88 // GET DATA: nothing captured
#define SW_INS_NOT_SUPPORTED        0x6D00 // anything else

// ISO 7816-4 CLA values an EMV contactless kernel uses. Also the guard that
// keeps an official-firmware build safe: there ReceivedData carries PCB+INF,
// so inf[0] is a PCB (0x02/0x03) and matches neither.
#define CLA_STANDARD    0x00 // interindustry: SELECT, READ RECORD
#define CLA_PROPRIETARY 0x80 // EMV-specific: GPO, GENERATE AC, GET DATA

// SELECT P1: by DF name. EMV never selects by file identifier.
#define P1_SELECT_BY_NAME 0x04

#define INS_SELECT        0xA4
#define INS_GET_DATA      0xCA
#define INS_GENERATE_AC   0xAE
#define INS_READ_RECORD   0xB2
#define INS_GET_PO        0xA8

// "2PAY.SYS.DDF01", the PPSE directory name every contactless EMV card has.
static const uint8_t k_ppse_name[] = {
    0x32, 0x50, 0x41, 0x59, 0x2E, 0x53, 0x59,
    0x53, 0x2E, 0x44, 0x44, 0x46, 0x30, 0x31,
};

#define EMV_AID_MIN_LEN 5 // EMV: an AID is 5..16 bytes

static bool emv_emit_sw(uint16_t sw, uint8_t* out, size_t out_cap, size_t* out_len) {
    if(out_cap < 2) return false;
    out[0] = (uint8_t)(sw >> 8);
    out[1] = (uint8_t)(sw & 0xFF);
    *out_len = 2;
    return true;
}

// A captured blob, verbatim, or `sw` when nothing was captured for it. A blob
// that does not fit `out_cap` emits nothing at all rather than a truncated
// response a terminal would mis-frame.
static bool emv_emit_replay(
    const uint8_t* src,
    uint16_t len,
    uint16_t sw,
    uint8_t* out,
    size_t out_cap,
    size_t* out_len) {
    if(len == 0) return emv_emit_sw(sw, out, out_cap, out_len);
    if(len > out_cap) return false;
    memcpy(out, src, len);
    *out_len = len;
    return true;
}

/* ------------------------------ framing ------------------------------- */

uint8_t emv_emu_pcb(uint8_t block_num, bool chain) {
    return (uint8_t)(0x02 | (block_num & 1) | (chain ? 0x10 : 0));
}

uint16_t emv_emu_fsc(uint8_t t0) {
    const uint8_t fsci = (uint8_t)(t0 & 0x0F);
    if(fsci >= 13) return 32; // RFU: ISO14443-4 default
    if(fsci < 5) return (uint16_t)(fsci * 8 + 16);
    if(fsci == 5) return 64;
    if(fsci == 6) return 96;
    return (uint16_t)(128U << (fsci - 7));
}

size_t emv_emu_chunk_max(uint16_t fsc) {
    // PCB (1) + CRC-A (2) come out of every frame; no CID byte.
    return fsc >= 4 ? (size_t)(fsc - 3) : 0;
}

/* ------------------------------ dispatch ------------------------------ */

bool emv_emu_apdu(
    const EmvReplay* r,
    const uint8_t* inf,
    size_t inf_len,
    uint8_t* out,
    size_t out_cap,
    size_t* out_len) {
    if(!r || !inf || !out || !out_len) return false;
    if(inf_len < 4) return false; // CLA INS P1 P2 is the shortest possible case

    const uint8_t cla = inf[0];
    // Doubles as the official-firmware guard: there ReceivedData carries the
    // ISO-DEP prologue, so inf[0] is a PCB (0x02/0x03), not a CLA. Rejecting it
    // degrades to transport-only emulation instead of emitting garbage.
    if(cla != CLA_STANDARD && cla != CLA_PROPRIETARY) return false;
    const uint8_t ins = inf[1];
    const uint8_t p1 = inf[2];
    const uint8_t p2 = inf[3];

    // Short-form case analysis (CLA INS P1 P2 [Lc Data] [Le]), the same one
    // Momentum's own type_4_tag_listener_handle_apdu() runs.
    const uint8_t* body = inf + 4;
    size_t body_len = inf_len - 4;
    const uint8_t* data = NULL;
    size_t lc = 0;
    if(body_len == 1) {
        // Le only, no data field.
    } else if(body_len > 1) {
        if(body[0] == 0) return false; // extended Lc, unsupported
        lc = body[0];
        if(body_len - 1 < lc) return false; // data field shorter than Lc
        if(body_len - 1 != lc && body_len - 1 != lc + 1) return false;
        data = body + 1;
    }

    if(cla == CLA_STANDARD && ins == INS_SELECT) {
        if(p1 != P1_SELECT_BY_NAME) {
            return emv_emit_sw(SW_INCORRECT_P1_P2, out, out_cap, out_len);
        }
        if(lc == sizeof(k_ppse_name) && memcmp(data, k_ppse_name, sizeof(k_ppse_name)) == 0) {
            return emv_emit_replay(
                r->ppse, r->ppse_len, SW_FILE_NOT_FOUND, out, out_cap, out_len);
        }
        // EMV allows a right-truncated AID: any prefix of >= 5 bytes of the
        // captured application's AID selects it.
        if(r->adf_aid_len > 0 && lc >= EMV_AID_MIN_LEN && lc <= r->adf_aid_len &&
           memcmp(data, r->adf_aid, lc) == 0) {
            return emv_emit_replay(r->adf, r->adf_len, SW_FILE_NOT_FOUND, out, out_cap, out_len);
        }
        return emv_emit_sw(SW_FILE_NOT_FOUND, out, out_cap, out_len);
    }

    if(cla == CLA_PROPRIETARY && ins == INS_GET_PO) {
        // The command's PDOL data is ignored on purpose: the captured AIP/AFL
        // response is a static property of the card, not of the terminal's DOL.
        return emv_emit_replay(r->gpo, r->gpo_len, SW_FILE_NOT_FOUND, out, out_cap, out_len);
    }

    if(cla == CLA_STANDARD && ins == INS_READ_RECORD) {
        const uint8_t sfi = (uint8_t)(p2 >> 3);
        const uint8_t num = p1; // 0 means "first or only" -- not what we captured
        if(num != 0) {
            for(uint8_t i = 0; i < r->rec_count && i < EMV_REPLAY_MAX_RECORDS; i++) {
                if(r->rec[i].sfi == sfi && r->rec[i].num == num) {
                    return emv_emit_replay(
                        r->rec[i].data,
                        r->rec[i].len,
                        SW_RECORD_NOT_FOUND,
                        out,
                        out_cap,
                        out_len);
                }
            }
        }
        return emv_emit_sw(SW_RECORD_NOT_FOUND, out, out_cap, out_len);
    }

    if(cla == CLA_PROPRIETARY && ins == INS_GENERATE_AC) {
        return emv_emit_sw(SW_CONDITIONS_NOT_SATISFIED, out, out_cap, out_len);
    }

    if(cla == CLA_PROPRIETARY && ins == INS_GET_DATA) {
        return emv_emit_sw(SW_REF_DATA_NOT_FOUND, out, out_cap, out_len);
    }

    return emv_emit_sw(SW_INS_NOT_SUPPORTED, out, out_cap, out_len);
}
