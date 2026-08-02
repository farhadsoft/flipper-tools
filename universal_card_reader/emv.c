/*
 * Read-only EMV application-layer reader.
 * ------------------------------------------------------------
 * Runs the minimum command chain needed to surface PAN, expiry, cardholder
 * name, AID(s), application label and transaction log from a contactless
 * bank card: SELECT PPSE -> SELECT AID -> GET PROCESSING OPTIONS ->
 * READ RECORD across the AFL -> (optional) transaction log GET DATA/READ
 * RECORD. Every step is read-only: the only INS bytes issued anywhere below
 * are SELECT (A4), GET PROCESSING OPTIONS (80 A8), READ RECORD (00 B2) and
 * GET DATA (80 CA); GET RESPONSE (00 C0), used only to satisfy a 61xx status
 * word, is the sole exception.
 *
 * Called from inside the NfcPoller callback (NfcWorker thread, fixed 8 KB
 * stack). No stack array here exceeds 64 bytes; anything larger (the GET
 * PROCESSING OPTIONS command, which carries the PDOL data) is heap-allocated
 * for the duration of that one call.
 */

#include "emv.h"

#include <furi.h>
#include <toolbox/bit_buffer.h>
#include <flipper_format/flipper_format.h>
#include <storage/storage.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>

#define TAG "UniEmv"

#define EMV_SW_OK 0x9000 // APDU status word: success

#define EMV_TAG_PAN              0x5A
#define EMV_TAG_TRACK2_EQUIV     0x57
#define EMV_TAG_APP_LABEL        0x50
#define EMV_TAG_EXPIRY           0x5F24
#define EMV_TAG_CARDHOLDER_NAME  0x5F20
#define EMV_TAG_SERVICE_CODE     0x5F30
#define EMV_TAG_ISSUER_COUNTRY   0x5F28
#define EMV_TAG_CARD_SEQ_NUM     0x5F34
#define EMV_TAG_APP_PREF_NAME    0x9F12
#define EMV_TAG_TRACK2_MAGSTRIPE 0x9F6B

/* ------------------------------ BER-TLV ------------------------------ */

// Parses a BER-TLV tag at *p, advancing *p past it. Multi-byte tags (low 5
// bits of the first byte all set) continue while bit 8 of each following
// byte is set, up to 3 tag bytes total, packed big-endian into *tag.
static bool ber_tag(const uint8_t** p, const uint8_t* end, uint32_t* tag, bool* constructed) {
    if(*p >= end) return false;
    uint8_t first = **p;
    uint32_t t = first;
    *constructed = (first & 0x20) != 0;
    (*p)++;

    if((first & 0x1F) == 0x1F) {
        uint8_t count = 1;
        while(*p < end && count < 3) {
            uint8_t b = **p;
            t = (t << 8) | b;
            (*p)++;
            count++;
            if((b & 0x80) == 0) break;
        }
    }
    *tag = t;
    return true;
}

// Parses a BER-TLV length at *p, advancing *p past it. Malformed length
// bytes (anything but definite short/0x81/0x82 form) stop the level.
static bool ber_len(const uint8_t** p, const uint8_t* end, size_t* len) {
    if(*p >= end) return false;
    uint8_t b = **p;
    (*p)++;

    if(b < 0x80) {
        *len = b;
        return true;
    }
    if(b == 0x81) {
        if(*p >= end) return false;
        *len = **p;
        (*p)++;
        return true;
    }
    if(b == 0x82) {
        if((size_t)(end - *p) < 2) return false;
        *len = ((size_t)(*p)[0] << 8) | (*p)[1];
        *p += 2;
        return true;
    }
    return false;
}

// Reads one TLV at *p, advancing *p to the next one. Never returns a value
// slice that runs past `end`.
static bool tlv_next(
    const uint8_t** p,
    const uint8_t* end,
    uint32_t* tag,
    bool* constructed,
    const uint8_t** val,
    size_t* val_len) {
    const uint8_t* cur = *p;
    if(!ber_tag(&cur, end, tag, constructed)) return false;

    size_t len;
    if(!ber_len(&cur, end, &len)) return false;
    if(len > (size_t)(end - cur)) return false;

    *val = cur;
    *val_len = len;
    *p = cur + len;
    return true;
}

#define TLV_FIND_MAX_DEPTH 6

static bool tlv_find_at(
    const uint8_t* data,
    size_t len,
    uint32_t tag,
    const uint8_t** val,
    size_t* val_len,
    uint8_t depth) {
    if(depth > TLV_FIND_MAX_DEPTH) return false;

    const uint8_t* p = data;
    const uint8_t* end = data + len;
    while(p < end) {
        uint32_t t;
        bool constructed;
        const uint8_t* v;
        size_t vlen;
        if(!tlv_next(&p, end, &t, &constructed, &v, &vlen)) break;

        if(t == tag) {
            *val = v;
            *val_len = vlen;
            return true;
        }
        if(constructed && tlv_find_at(v, vlen, tag, val, val_len, (uint8_t)(depth + 1))) {
            return true;
        }
    }
    return false;
}

// Depth-first search for `tag`, descending only into constructed nodes.
static bool
    tlv_find(const uint8_t* data, size_t len, uint32_t tag, const uint8_t** val, size_t* val_len) {
    return tlv_find_at(data, len, tag, val, val_len, 0);
}

// A DOL entry is a tag/length pair with no value (the value lives elsewhere:
// it is either supplied by the terminal, as in a PDOL, or sliced out of a
// separately-fetched record, as in a Log Format DOL). Same tag/length
// encoding as a TLV.
static bool dol_next(const uint8_t** p, const uint8_t* end, uint32_t* tag, size_t* len) {
    bool constructed;
    if(!ber_tag(p, end, tag, &constructed)) return false;
    return ber_len(p, end, len);
}

/* ------------------------------ helpers ------------------------------- */

static const char EMV_HEX_DIGITS[] = "0123456789ABCDEF";

static void hex_str(const uint8_t* d, size_t len, char* out, size_t out_cap) {
    size_t n = 0;
    for(size_t i = 0; i < len && n + 2 < out_cap; i++) {
        out[n++] = EMV_HEX_DIGITS[d[i] >> 4];
        out[n++] = EMV_HEX_DIGITS[d[i] & 0x0F];
    }
    out[n] = '\0';
}

// Bounded byte copy used for the application label (tag 50): truncates to
// `cap - 1` bytes rather than the field-specific caps below, since EMV
// labels are already short, printable ASCII.
static void emv_set_label(char* label, size_t cap, const uint8_t* v, size_t len) {
    size_t n = len < cap - 1 ? len : cap - 1;
    memcpy(label, v, n);
    label[n] = '\0';
}

/* --------------------------- GPO data (PDOL) --------------------------- */

// Standard terminal defaults for the tags a PDOL commonly requests. A tag
// absent from this table is emitted as zero bytes.
typedef struct {
    uint32_t tag;
    const uint8_t* val;
    uint8_t len;
} PdolDefault;

static const uint8_t k_9f66[] = {0x36, 0x00, 0x00, 0x00};
static const uint8_t k_9f02[] = {0x00, 0x00, 0x00, 0x00, 0x01, 0x00};
static const uint8_t k_9f03[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t k_9f1a[] = {0x08, 0x26};
static const uint8_t k_5f2a[] = {0x08, 0x26};
static const uint8_t k_95[] = {0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t k_9a[] = {0x26, 0x01, 0x01};
static const uint8_t k_9c[] = {0x00};
static const uint8_t k_9f37[] = {0x82, 0x3D, 0xDE, 0x7A};
static const uint8_t k_9f35[] = {0x22};

static const PdolDefault pdol_defaults[] = {
    {0x9F66, k_9f66, sizeof(k_9f66)},
    {0x9F02, k_9f02, sizeof(k_9f02)},
    {0x9F03, k_9f03, sizeof(k_9f03)},
    {0x9F1A, k_9f1a, sizeof(k_9f1a)},
    {0x5F2A, k_5f2a, sizeof(k_5f2a)},
    {0x95, k_95, sizeof(k_95)},
    {0x9A, k_9a, sizeof(k_9a)},
    {0x9C, k_9c, sizeof(k_9c)},
    {0x9F37, k_9f37, sizeof(k_9f37)},
    {0x9F35, k_9f35, sizeof(k_9f35)},
};

// Emits the terminal's default value for `tag`, left-zero-padded or
// left-truncated to exactly `req_len` bytes (an unlisted tag is all zero).
static void pdol_emit(uint32_t tag, size_t req_len, uint8_t* out) {
    if(req_len == 0) return;

    const uint8_t* src = NULL;
    size_t src_len = 0;
    for(size_t i = 0; i < COUNT_OF(pdol_defaults); i++) {
        if(pdol_defaults[i].tag == tag) {
            src = pdol_defaults[i].val;
            src_len = pdol_defaults[i].len;
            break;
        }
    }

    if(src_len >= req_len) {
        memcpy(out, src + (src_len - req_len), req_len);
    } else {
        size_t pad = req_len - src_len;
        memset(out, 0, pad);
        if(src_len > 0) memcpy(out + pad, src, src_len);
    }
}

// Walks `pdol` as a DOL, emitting the terminal's default value for each
// requested tag at its requested length. Stops once `cap` bytes are filled.
static size_t emv_build_pdol_data(const uint8_t* pdol, size_t pdol_len, uint8_t* out, size_t cap) {
    const uint8_t* p = pdol;
    const uint8_t* end = pdol + pdol_len;
    size_t out_len = 0;

    while(p < end && out_len < cap) {
        uint32_t tag;
        size_t len;
        if(!dol_next(&p, end, &tag, &len)) break;
        if(len > cap - out_len) len = cap - out_len;
        pdol_emit(tag, len, out + out_len);
        out_len += len;
    }
    return out_len;
}

/* -------------------------- field harvesting --------------------------- */

// Tag 5A: packed BCD PAN, terminated by nibble 0xF, capped at 19 digits.
static void emv_harvest_pan_bcd(const uint8_t* v, size_t len, char* pan, size_t pan_cap) {
    size_t n = 0;
    for(size_t i = 0; i < len * 2 && n < 19 && n + 1 < pan_cap; i++) {
        uint8_t nibble = (i % 2 == 0) ? (v[i / 2] >> 4) : (v[i / 2] & 0x0F);
        if(nibble == 0x0F) break;
        pan[n++] = (char)('0' + nibble);
    }
    pan[n] = '\0';
}

// Tag 5F20: cardholder name, printable ASCII only, trailing spaces trimmed.
static void emv_copy_name(const uint8_t* v, size_t len, char* name, size_t cap) {
    size_t n = 0;
    for(size_t i = 0; i < len && n + 1 < cap; i++) {
        if(v[i] >= 0x20 && v[i] <= 0x7E) name[n++] = (char)v[i];
    }
    while(n > 0 && name[n - 1] == ' ') n--;
    name[n] = '\0';
}

// Tags 57 / 9F6B: Track 2 (equivalent / MSD). PAN digits run up to the 0xD
// separator nibble; the four digits after it are YYMM, re-ordered to
// "MM/YY". Either field is only written if still empty.
static void emv_harvest_track2(const uint8_t* v, size_t len, EmvData* out) {
    size_t total = len * 2;
    char pan_digits[20];
    size_t pan_n = 0;
    size_t i = 0;

    for(; i < total; i++) {
        uint8_t nibble = (i % 2 == 0) ? (v[i / 2] >> 4) : (v[i / 2] & 0x0F);
        if(nibble == 0x0D) break;
        if(nibble == 0x0F) {
            i = total;
            break;
        }
        if(pan_n < 19) pan_digits[pan_n++] = (char)('0' + nibble);
    }
    pan_digits[pan_n] = '\0';
    bool found_sep = (i < total);

    char yymm[4];
    size_t yymm_n = 0;
    if(found_sep) {
        for(i = i + 1; i < total && yymm_n < 4; i++) {
            uint8_t nibble = (i % 2 == 0) ? (v[i / 2] >> 4) : (v[i / 2] & 0x0F);
            if(nibble > 9) break;
            yymm[yymm_n++] = (char)('0' + nibble);
        }
    }
    if(out->pan[0] == '\0' && pan_n > 0) {
        memcpy(out->pan, pan_digits, pan_n + 1);
    }
    if(out->expiry[0] == '\0' && yymm_n == 4) {
        out->expiry[0] = yymm[2];
        out->expiry[1] = yymm[3];
        out->expiry[2] = '/';
        out->expiry[3] = yymm[0];
        out->expiry[4] = yymm[1];
        out->expiry[5] = '\0';
    }
    // Store raw Track 2 equivalent data for save/emulation
    if(out->track2_len == 0 && len > 0 && len <= EMV_TRACK2_MAX_LEN) {
        memcpy(out->track2, v, len);
        out->track2_len = (uint8_t)len;
    }
}

static void emv_take_expiry(const uint8_t* val, size_t val_len, EmvData* out) {
    if(out->expiry[0] != '\0' || val_len < 2) return;
    snprintf(out->expiry, sizeof(out->expiry), "%02X/%02X", (unsigned)val[1], (unsigned)val[0]);
}

// Service code, 3 digits BCD
static void emv_take_service_code(const uint8_t* val, size_t val_len, EmvData* out) {
    if(out->service_code[0] != '\0' || val_len < 2) return;
    snprintf(
        out->service_code,
        sizeof(out->service_code),
        "%02X%01X",
        (unsigned)val[0],
        (unsigned)(val[1] >> 4));
}

static void emv_take_app_pref_name(const uint8_t* val, size_t val_len, EmvData* out) {
    if(out->app_pref_name[0] != '\0') return;
    size_t n = val_len < EMV_APP_PREF_NAME_LEN ? val_len : EMV_APP_PREF_NAME_LEN;
    memcpy(out->app_pref_name, val, n);
    out->app_pref_name[n] = '\0';
}

// Issuer country code, ISO 3166, BCD 2 bytes
static void emv_take_issuer_country(const uint8_t* val, size_t val_len, EmvData* out) {
    if(out->issuer_country[0] != '\0' || val_len < 2) return;
    unsigned cc = (unsigned)(val[0] << 8) | val[1];
    if(cc > 999) cc = 999;
    snprintf(out->issuer_country, sizeof(out->issuer_country), "%03u", cc);
}

// Card sequence number
static void emv_take_card_seq(const uint8_t* val, size_t val_len, EmvData* out) {
    if(out->card_seq_num[0] != '\0' || val_len < 1) return;
    size_t n = val_len < EMV_CARD_SEQ_NUM_LEN ? val_len : EMV_CARD_SEQ_NUM_LEN;
    for(size_t k = 0; k < n; k++) {
        out->card_seq_num[k] = (char)('0' + ((val[k] >> 4) & 0x0F));
    }
    out->card_seq_num[n] = '\0';
}

#define EMV_HARVEST_MAX_DEPTH 6

// Runs over any record or template, depth-first, filling every field that is
// still empty. The first occurrence of a tag anywhere in the chain wins.
static void emv_harvest_level(const uint8_t* data, size_t len, EmvData* out, uint8_t depth) {
    if(depth > EMV_HARVEST_MAX_DEPTH) return;

    const uint8_t* p = data;
    const uint8_t* end = data + len;
    while(p < end) {
        uint32_t tag;
        bool constructed;
        const uint8_t* val;
        size_t val_len;
        if(!tlv_next(&p, end, &tag, &constructed, &val, &val_len)) break;

        switch(tag) {
        case EMV_TAG_PAN:
            if(out->pan[0] == '\0') emv_harvest_pan_bcd(val, val_len, out->pan, sizeof(out->pan));
            break;
        case EMV_TAG_EXPIRY:
            emv_take_expiry(val, val_len, out);
            break;
        case EMV_TAG_CARDHOLDER_NAME:
            if(out->name[0] == '\0') emv_copy_name(val, val_len, out->name, sizeof(out->name));
            break;
        case EMV_TAG_TRACK2_EQUIV:
        case EMV_TAG_TRACK2_MAGSTRIPE:
            emv_harvest_track2(val, val_len, out);
            break;
        case EMV_TAG_APP_LABEL:
            if(out->label[0] == '\0') emv_set_label(out->label, sizeof(out->label), val, val_len);
            break;
        case EMV_TAG_SERVICE_CODE:
            emv_take_service_code(val, val_len, out);
            break;
        case EMV_TAG_APP_PREF_NAME:
            emv_take_app_pref_name(val, val_len, out);
            break;
        case EMV_TAG_ISSUER_COUNTRY:
            emv_take_issuer_country(val, val_len, out);
            break;
        case EMV_TAG_CARD_SEQ_NUM:
            emv_take_card_seq(val, val_len, out);
            break;
        default:
            break;
        }

        if(constructed) emv_harvest_level(val, val_len, out, (uint8_t)(depth + 1));
    }
}

static void emv_harvest(const uint8_t* data, size_t len, EmvData* out) {
    emv_harvest_level(data, len, out, 0);
}

// Slices one raw (non-TLV) transaction log record according to the Log
// Format DOL: only tags 9A / 9F02 / 5F2A are captured, everything else is
// skipped over by its declared length.
static void emv_parse_log_record(
    const uint8_t* dol,
    size_t dol_len,
    const uint8_t* body,
    size_t body_len,
    EmvLogRow* row) {
    const uint8_t* p = dol;
    const uint8_t* end = dol + dol_len;
    size_t offset = 0;

    while(p < end) {
        uint32_t tag;
        size_t flen;
        if(!dol_next(&p, end, &tag, &flen)) break;
        if(offset + flen > body_len) break;

        const uint8_t* field = body + offset;
        switch(tag) {
        case 0x9A:
            if(flen >= 3) {
                memcpy(row->date, field, 3);
                row->has_date = true;
            }
            break;
        case 0x9F02:
            if(flen >= 6) {
                memcpy(row->amount, field, 6);
                row->has_amount = true;
            }
            break;
        case 0x5F2A:
            if(flen >= 2) {
                row->currency = ((uint16_t)field[0] << 8) | field[1];
                row->has_currency = true;
            }
            break;
        default:
            break;
        }
        offset += flen;
    }
}

/* --------------------------- APDU transport ---------------------------- */

// Sends one already-built APDU, waits for the response, and splits SW1SW2
// off it. A transport error or a response shorter than 2 bytes is reported
// as `false`/`sw = 0`; anything else is `true` regardless of the actual SW,
// so the caller decides what to do with it. `body`/`body_len` alias `rx`'s
// buffer, valid only until `rx` is reused.
//
// Card->reader I-block chaining is not supported by send_block; a chained
// response surfaces as Iso14443_4aErrorProtocol. Treated the same as any
// other transport error: "this step returned nothing", not a chain abort.
static bool emv_transceive(
    Iso14443_4aPoller* poller,
    BitBuffer* tx,
    BitBuffer* rx,
    const uint8_t* apdu,
    size_t apdu_len,
    const uint8_t** body,
    size_t* body_len,
    uint16_t* sw) {
    bit_buffer_reset(tx);
    bit_buffer_copy_bytes(tx, apdu, apdu_len);

    Iso14443_4aError err = iso14443_4a_poller_send_block(poller, tx, rx);
    if(err != Iso14443_4aErrorNone) {
        *sw = 0;
        return false;
    }

    size_t size = bit_buffer_get_size_bytes(rx);
    if(size < 2) {
        *sw = 0;
        return false;
    }

    const uint8_t* data = bit_buffer_get_data(rx);
    *sw = ((uint16_t)data[size - 2] << 8) | data[size - 1];
    *body = data;
    *body_len = size - 2;
    return true;
}

// Wraps emv_transceive with the two standard EMV continuations: a 61xx
// status word means "more data, GET RESPONSE"; a 6Cxx status word means
// "wrong Le, resend with the exact length". Each is followed at most once.
static bool emv_apdu(
    Iso14443_4aPoller* poller,
    BitBuffer* tx,
    BitBuffer* rx,
    const uint8_t* apdu,
    size_t apdu_len,
    const uint8_t** body,
    size_t* body_len,
    uint16_t* sw) {
    if(!emv_transceive(poller, tx, rx, apdu, apdu_len, body, body_len, sw)) return false;

    if((*sw >> 8) == 0x61) {
        uint8_t get_response[5] = {0x00, 0xC0, 0x00, 0x00, (uint8_t)(*sw & 0xFF)};
        return emv_transceive(
            poller, tx, rx, get_response, sizeof(get_response), body, body_len, sw);
    }

    if((*sw >> 8) == 0x6C && apdu_len > 0 && apdu_len <= 64) {
        uint8_t retry[64];
        memcpy(retry, apdu, apdu_len);
        retry[apdu_len - 1] = (uint8_t)(*sw & 0xFF);
        return emv_transceive(poller, tx, rx, retry, apdu_len, body, body_len, sw);
    }

    return true;
}

/* ---------------------------- command chain ---------------------------- */

#define EMV_PDOL_MAX    64 // max raw PDOL bytes copied out of the FCI
#define EMV_AFL_MAX     64 // max raw AFL bytes copied out of the GPO response
#define EMV_LOG_DOL_MAX 32 // max raw Log Format DOL bytes

static const uint8_t k_ppse_apdu[] = {
    0x00,
    0xA4,
    0x04,
    0x00,
    0x0E,
    0x32,
    0x50,
    0x41,
    0x59,
    0x2E,
    0x53,
    0x59,
    0x53,
    0x2E,
    0x44,
    0x44,
    0x46,
    0x30,
    0x31, // "2PAY.SYS.DDF01"
    0x00,
};

static void ppse_add_aid(EmvData* out, const uint8_t* aid, size_t aid_len) {
    if(out->aid_count >= EMV_MAX_AIDS) return;
    size_t n = aid_len < EMV_AID_MAX_LEN ? aid_len : EMV_AID_MAX_LEN;
    uint8_t idx = out->aid_count;
    memcpy(out->aid[idx], aid, n);
    out->aid_len[idx] = (uint8_t)n;
    out->aid_count++;
}

// Depth-first walk of the PPSE FCI collecting every `61` application
// template's AID (`4F`) and, from the first template that carries one, the
// application label (`50`).
static void ppse_collect_templates(const uint8_t* data, size_t len, EmvData* out, uint8_t depth) {
    if(depth > TLV_FIND_MAX_DEPTH) return;

    const uint8_t* p = data;
    const uint8_t* end = data + len;
    while(p < end) {
        uint32_t tag;
        bool constructed;
        const uint8_t* val;
        size_t val_len;
        if(!tlv_next(&p, end, &tag, &constructed, &val, &val_len)) break;

        if(tag == 0x61) {
            const uint8_t* aid_val;
            size_t aid_len;
            if(tlv_find(val, val_len, 0x4F, &aid_val, &aid_len)) {
                ppse_add_aid(out, aid_val, aid_len);
            }
            const uint8_t* label_val;
            size_t label_len;
            if(out->label[0] == '\0' && tlv_find(val, val_len, 0x50, &label_val, &label_len)) {
                emv_set_label(out->label, sizeof(out->label), label_val, label_len);
            }
        } else if(constructed) {
            ppse_collect_templates(val, val_len, out, (uint8_t)(depth + 1));
        }
    }
}

// SELECT PPSE (2PAY.SYS.DDF01): the contactless directory every EMV card
// answers with its list of payment applications.
static bool
    emv_select_ppse(Iso14443_4aPoller* poller, BitBuffer* tx, BitBuffer* rx, EmvData* out) {
    const uint8_t* body;
    size_t body_len;
    uint16_t sw = 0;
    bool ok = emv_apdu(poller, tx, rx, k_ppse_apdu, sizeof(k_ppse_apdu), &body, &body_len, &sw);

    if(ok && sw == EMV_SW_OK) {
        ppse_collect_templates(body, body_len, out, 0);
        if(out->aid_count == 0) {
            const uint8_t* aid_val;
            size_t aid_len;
            if(tlv_find(body, body_len, 0x4F, &aid_val, &aid_len)) {
                ppse_add_aid(out, aid_val, aid_len);
            }
        }
    }
    FURI_LOG_I(TAG, "PPSE sw=%04X aids=%u", (unsigned)sw, (unsigned)out->aid_count);
    if(!ok || sw != EMV_SW_OK || out->aid_count == 0) return false;

    out->ppse_ok = true;
    return true;
}

// SELECT AID: tries every AID PPSE offered, in order, and keeps the first
// that answers 9000. Also harvests the label/PDOL/log pointer out of that
// AID's FCI.
static bool emv_select_aid(
    Iso14443_4aPoller* poller,
    BitBuffer* tx,
    BitBuffer* rx,
    EmvData* out,
    uint8_t* pdol,
    size_t* pdol_len) {
    *pdol_len = 0;

    for(uint8_t i = 0; i < out->aid_count; i++) {
        uint8_t apdu[5 + EMV_AID_MAX_LEN + 1];
        size_t alen = out->aid_len[i];
        apdu[0] = 0x00;
        apdu[1] = 0xA4;
        apdu[2] = 0x04;
        apdu[3] = 0x00;
        apdu[4] = (uint8_t)alen;
        memcpy(&apdu[5], out->aid[i], alen);
        apdu[5 + alen] = 0x00;

        const uint8_t* body;
        size_t body_len;
        uint16_t sw = 0;
        bool ok = emv_apdu(poller, tx, rx, apdu, 5 + alen + 1, &body, &body_len, &sw);

        char aid_hex[EMV_AID_MAX_LEN * 2 + 1];
        hex_str(out->aid[i], alen, aid_hex, sizeof(aid_hex));
        FURI_LOG_I(TAG, "SELECT AID %s sw=%04X", aid_hex, (unsigned)sw);

        if(!ok || sw != EMV_SW_OK) continue;

        out->aid_selected = true;
        out->aid_selected_idx = i;

        const uint8_t* v;
        size_t vlen;
        if(tlv_find(body, body_len, 0x50, &v, &vlen)) {
            emv_set_label(out->label, sizeof(out->label), v, vlen); // overwrite
        } else if(out->label[0] == '\0' && tlv_find(body, body_len, 0x9F12, &v, &vlen)) {
            emv_set_label(out->label, sizeof(out->label), v, vlen);
        }
        if(tlv_find(body, body_len, 0x9F38, &v, &vlen)) {
            size_t n = vlen < EMV_PDOL_MAX ? vlen : EMV_PDOL_MAX;
            memcpy(pdol, v, n);
            *pdol_len = n;
        }
        if(tlv_find(body, body_len, 0x9F4D, &v, &vlen) && vlen >= 2) {
            out->log_sfi = v[0];
            out->log_count = v[1];
        }
        return true;
    }
    return false;
}

// GET PROCESSING OPTIONS. Builds the command data from the AID's PDOL (or
// `83 00` when it has none), then extracts the AFL from either response
// format: format 1 (`80`) carries AIP+AFL as one primitive value; format 2
// (`77`) is constructed, so the AFL is pulled out via `94` and the whole
// template is also run through the harvester, because Visa qVSDC puts
// `57`/`5A` directly in it with an empty AFL. `afl_buf` receives a copy of
// the raw AFL bytes: they alias `rx`'s buffer otherwise, which the next
// APDU call overwrites.
static void emv_gpo(
    Iso14443_4aPoller* poller,
    BitBuffer* tx,
    BitBuffer* rx,
    EmvData* out,
    const uint8_t* pdol,
    size_t pdol_len,
    uint8_t* afl_buf,
    size_t* afl_len) {
    *afl_len = 0;

    uint8_t pdol_data[EMV_PDOL_MAX];
    size_t pdol_data_len = emv_build_pdol_data(pdol, pdol_len, pdol_data, sizeof(pdol_data));

    // 80 A8 00 00 <Lc> 83 <len> <pdol_data...> 00 -- heap-allocated: with a
    // full PDOL this exceeds the 64-byte stack-array budget.
    size_t apdu_cap = 4 + 1 + 2 + EMV_PDOL_MAX + 1;
    uint8_t* apdu = malloc(apdu_cap);
    uint16_t sw = 0;
    bool ok = false;
    const uint8_t* body = NULL;
    size_t body_len = 0;

    if(apdu) {
        size_t n = 0;
        apdu[n++] = 0x80;
        apdu[n++] = 0xA8;
        apdu[n++] = 0x00;
        apdu[n++] = 0x00;
        size_t lc_index = n++;
        apdu[n++] = 0x83;
        apdu[n++] = (uint8_t)pdol_data_len;
        memcpy(&apdu[n], pdol_data, pdol_data_len);
        n += pdol_data_len;
        apdu[lc_index] = (uint8_t)(n - lc_index - 1);
        apdu[n++] = 0x00;

        ok = emv_apdu(poller, tx, rx, apdu, n, &body, &body_len, &sw);
        free(apdu);
    }

    if(ok && sw == EMV_SW_OK && body_len > 0) {
        uint32_t tag;
        bool constructed;
        const uint8_t* val;
        size_t val_len;
        const uint8_t* p = body;
        if(tlv_next(&p, body + body_len, &tag, &constructed, &val, &val_len)) {
            if(tag == 0x80 && val_len >= 2) {
                size_t m = (val_len - 2) < EMV_AFL_MAX ? (val_len - 2) : EMV_AFL_MAX;
                memcpy(afl_buf, val + 2, m);
                *afl_len = m;
            } else if(tag == 0x77) {
                emv_harvest(val, val_len, out);
                const uint8_t* afl_val;
                size_t afl_val_len;
                if(tlv_find(val, val_len, 0x94, &afl_val, &afl_val_len)) {
                    size_t m = afl_val_len < EMV_AFL_MAX ? afl_val_len : EMV_AFL_MAX;
                    memcpy(afl_buf, afl_val, m);
                    *afl_len = m;
                }
            }
        }
        out->gpo_ok = true;
    }

    FURI_LOG_I(TAG, "GPO sw=%04X afl=%u", (unsigned)sw, (unsigned)(*afl_len / 4));
}

// READ RECORD over every group in the AFL, harvesting each successfully
// read record. Caps the total number of records read so a misbehaving card
// cannot stall the poller indefinitely.
static void emv_read_records(
    Iso14443_4aPoller* poller,
    BitBuffer* tx,
    BitBuffer* rx,
    EmvData* out,
    const uint8_t* afl,
    size_t afl_len) {
    uint8_t total_read = 0;

    for(size_t i = 0; i + 4 <= afl_len && total_read < 24; i += 4) {
        const uint8_t* g = afl + i;
        uint8_t sfi = g[0] >> 3;
        uint8_t first = g[1];
        uint8_t last = g[2];
        if(sfi == 0 || sfi == 31 || first == 0 || last < first) continue;

        // rec is widened past uint8_t so `rec <= last` cannot wrap when
        // last == 255.
        for(uint16_t rec = first; rec <= last && total_read < 24; rec++) {
            uint8_t apdu[5] = {0x00, 0xB2, (uint8_t)rec, (uint8_t)((sfi << 3) | 0x04), 0x00};
            const uint8_t* body;
            size_t body_len = 0;
            uint16_t sw = 0;
            bool ok = emv_apdu(poller, tx, rx, apdu, sizeof(apdu), &body, &body_len, &sw);
            total_read++;

            FURI_LOG_I(
                TAG,
                "record sfi=%u rec=%u sw=%04X len=%u",
                (unsigned)sfi,
                (unsigned)rec,
                (unsigned)sw,
                (unsigned)body_len);

            if(ok && sw == EMV_SW_OK) emv_harvest(body, body_len, out);
        }
    }
}

static const uint8_t k_get_log_format_apdu[] = {0x80, 0xCA, 0x9F, 0x4F, 0x00};

// Transaction log: GET DATA for the Log Format DOL, then READ RECORD each
// log entry and slice it by that DOL. A log record is raw concatenated
// fields, not TLV.
static void
    emv_read_log(Iso14443_4aPoller* poller, BitBuffer* tx, BitBuffer* rx, EmvData* out) {
    if(!(out->log_count > 0 && out->log_sfi >= 1 && out->log_sfi <= 30)) return;

    const uint8_t* body;
    size_t body_len = 0;
    uint16_t sw = 0;
    if(!emv_apdu(
           poller,
           tx,
           rx,
           k_get_log_format_apdu,
           sizeof(k_get_log_format_apdu),
           &body,
           &body_len,
           &sw) ||
       sw != EMV_SW_OK) {
        return;
    }

    const uint8_t* dol_val;
    size_t dol_val_len;
    if(!tlv_find(body, body_len, 0x9F4F, &dol_val, &dol_val_len)) return;

    uint8_t dol[EMV_LOG_DOL_MAX];
    size_t dol_len = dol_val_len < EMV_LOG_DOL_MAX ? dol_val_len : EMV_LOG_DOL_MAX;
    memcpy(dol, dol_val, dol_len);

    uint8_t max_rec = out->log_count < EMV_MAX_LOG_ROWS ? out->log_count : EMV_MAX_LOG_ROWS;
    for(uint8_t rec = 1; rec <= max_rec; rec++) {
        uint8_t apdu[5] = {0x00, 0xB2, rec, (uint8_t)((out->log_sfi << 3) | 0x04), 0x00};
        const uint8_t* rbody;
        size_t rbody_len = 0;
        uint16_t rsw = 0;
        bool ok = emv_apdu(poller, tx, rx, apdu, sizeof(apdu), &rbody, &rbody_len, &rsw);

        FURI_LOG_I(
            TAG,
            "record sfi=%u rec=%u sw=%04X len=%u",
            (unsigned)out->log_sfi,
            (unsigned)rec,
            (unsigned)rsw,
            (unsigned)rbody_len);

        if(!ok || rsw != EMV_SW_OK) continue;
        emv_parse_log_record(dol, dol_len, rbody, rbody_len, &out->log[out->log_rows]);
        out->log_rows++;
    }
}

/* ----------------------- fallback: well-known AIDs ----------------------- */

// Tried in order when SELECT PPSE returns anything other than 9000 with >= 1
// AID. Covers the six major payment networks and their sub-applications; a
// card that answers 9000 to any of these has a live EMV application even if
// it does not expose its PPSE directory.
typedef struct {
    const uint8_t* aid;
    uint8_t len;
} KnownAid;

static const uint8_t k_aid_visa[] = {0xA0, 0x00, 0x00, 0x00, 0x03, 0x10, 0x10};
static const uint8_t k_aid_visa_debit[] = {0xA0, 0x00, 0x00, 0x00, 0x03, 0x20, 0x10};
static const uint8_t k_aid_visa_electron[] = {0xA0, 0x00, 0x00, 0x00, 0x03, 0x20, 0x20};
static const uint8_t k_aid_mc_credit[] = {0xA0, 0x00, 0x00, 0x00, 0x04, 0x10, 0x10};
static const uint8_t k_aid_mc_debit[] = {0xA0, 0x00, 0x00, 0x00, 0x04, 0x30, 0x60};
static const uint8_t k_aid_amex[] = {0xA0, 0x00, 0x00, 0x00, 0x25, 0x01, 0x08, 0x01};
static const uint8_t k_aid_discover[] = {0xA0, 0x00, 0x00, 0x01, 0x52, 0x30, 0x10};
static const uint8_t k_aid_jcb[] = {0xA0, 0x00, 0x00, 0x00, 0x65, 0x10, 0x10};
static const uint8_t k_aid_unionpay[] = {0xA0, 0x00, 0x00, 0x03, 0x33, 0x01, 0x01, 0x01};
static const uint8_t k_aid_visa_interlink[] = {0xA0, 0x00, 0x00, 0x00, 0x03, 0x60, 0x10};

static const KnownAid k_known_aids[] = {
    {k_aid_visa, sizeof(k_aid_visa)},
    {k_aid_visa_debit, sizeof(k_aid_visa_debit)},
    {k_aid_visa_electron, sizeof(k_aid_visa_electron)},
    {k_aid_visa_interlink, sizeof(k_aid_visa_interlink)},
    {k_aid_mc_credit, sizeof(k_aid_mc_credit)},
    {k_aid_mc_debit, sizeof(k_aid_mc_debit)},
    {k_aid_amex, sizeof(k_aid_amex)},
    {k_aid_discover, sizeof(k_aid_discover)},
    {k_aid_jcb, sizeof(k_aid_jcb)},
    {k_aid_unionpay, sizeof(k_aid_unionpay)},
};

// Tries every well-known AID when PPSE yielded nothing. Populates out->aid[]
// with the first (or all) that answer 9000, so emv_select_aid() can pick it
// up on the next pass. Returns true when at least one AID was added.
static bool
    emv_try_fallback_aids(Iso14443_4aPoller* poller, BitBuffer* tx, BitBuffer* rx, EmvData* out) {
    FURI_LOG_I(TAG, "PPSE failed, trying %u known AIDs", (unsigned)COUNT_OF(k_known_aids));

    for(size_t i = 0; i < COUNT_OF(k_known_aids); i++) {
        const KnownAid* ka = &k_known_aids[i];
        uint8_t apdu[5 + EMV_AID_MAX_LEN + 1];
        apdu[0] = 0x00;
        apdu[1] = 0xA4;
        apdu[2] = 0x04;
        apdu[3] = 0x00;
        apdu[4] = ka->len;
        memcpy(&apdu[5], ka->aid, ka->len);
        apdu[5 + ka->len] = 0x00;

        const uint8_t* body;
        size_t body_len;
        uint16_t sw = 0;
        bool ok = emv_apdu(poller, tx, rx, apdu, 5 + ka->len + 1, &body, &body_len, &sw);

        char aid_hex[EMV_AID_MAX_LEN * 2 + 1];
        hex_str(ka->aid, ka->len, aid_hex, sizeof(aid_hex));
        FURI_LOG_I(TAG, "fallback SELECT AID %s sw=%04X", aid_hex, (unsigned)sw);

        if(ok && sw == EMV_SW_OK) {
            ppse_add_aid(out, ka->aid, ka->len);
            // Also harvest label/PDOL/log from this AID's FCI directly, since
            // emv_select_aid() will skip it (already selected, re-selecting may
            // give a different response on some cards).
            const uint8_t* v;
            size_t vlen;
            if(out->label[0] == '\0') {
                if(tlv_find(body, body_len, 0x50, &v, &vlen)) {
                    emv_set_label(out->label, sizeof(out->label), v, vlen);
                } else if(tlv_find(body, body_len, 0x9F12, &v, &vlen)) {
                    emv_set_label(out->label, sizeof(out->label), v, vlen);
                }
            }
            // Return on first hit: emv_select_aid() will re-select it and do
            // the full GPO/record chain from there.
            return true;
        }
    }
    return false;
}


bool emv_read(Iso14443_4aPoller* poller, EmvData* out) {
    memset(out, 0, sizeof(*out));

    BitBuffer* tx = bit_buffer_alloc(256);
    BitBuffer* rx = bit_buffer_alloc(256);

    // Step 1: try PPSE (the standard contactless directory).
    emv_select_ppse(poller, tx, rx, out);

    // Step 2: if PPSE yielded no AIDs, fall back to well-known AIDs.
    if(out->aid_count == 0) {
        emv_try_fallback_aids(poller, tx, rx, out);
    }

    // Step 3: select, GPO, records, log — same chain for both paths.
    if(out->aid_count > 0) {
        uint8_t pdol[EMV_PDOL_MAX];
        size_t pdol_len = 0;
        if(emv_select_aid(poller, tx, rx, out, pdol, &pdol_len)) {
            uint8_t afl_buf[EMV_AFL_MAX];
            size_t afl_len = 0;
            emv_gpo(poller, tx, rx, out, pdol, pdol_len, afl_buf, &afl_len);
            emv_read_records(poller, tx, rx, out, afl_buf, afl_len);
            emv_read_log(poller, tx, rx, out);
        }
    }

    // First 6 + **** + last 4; the unmasked PAN goes only to the debug log.
    char pan_masked[sizeof(out->pan) + 4];
    size_t pan_n = strlen(out->pan);
    if(pan_n >= 10) {
        snprintf(pan_masked, sizeof(pan_masked), "%.6s****%s", out->pan, out->pan + pan_n - 4);
    } else if(pan_n > 0) {
        snprintf(pan_masked, sizeof(pan_masked), "%s", out->pan);
    } else {
        snprintf(pan_masked, sizeof(pan_masked), "(none)");
    }
    FURI_LOG_I(
        TAG,
        "result: PAN %s exp %s label '%s' log %u/%u aids=%u",
        pan_masked,
        out->expiry[0] ? out->expiry : "(none)",
        out->label,
        (unsigned)out->log_rows,
        (unsigned)out->log_count,
        (unsigned)out->aid_count);
    FURI_LOG_D(TAG, "unmasked PAN %s", out->pan);

    bit_buffer_free(tx);
    bit_buffer_free(rx);
    return out->aid_selected;
}

/* -------------------------- save / load --------------------------- */

#define EMV_FILE_TYPE        "Universal EMV Card"
#define EMV_FILE_VERSION     3 // v3 adds the ISO14443-4A transport block (UID/ATQA/SAK/ATS)
#define EMV_FILE_MIN_VERSION 2 // v2 (financial fields only) still loads, just cannot emulate

static const struct {
    const char* key;
    size_t offset;
} k_emv_text_fields[] = {
    {"Label", offsetof(EmvData, label)},
    {"PAN", offsetof(EmvData, pan)},
    {"Expiry", offsetof(EmvData, expiry)},
    {"Cardholder", offsetof(EmvData, name)},
    {"Service Code", offsetof(EmvData, service_code)},
    {"App Preferred Name", offsetof(EmvData, app_pref_name)},
    {"Issuer Country", offsetof(EmvData, issuer_country)},
    {"Card Sequence", offsetof(EmvData, card_seq_num)},
};

// AIDs
static bool emv_save_aids(FlipperFormat* ff, const EmvData* data) {
    uint32_t aid_count = data->aid_count;
    if(!flipper_format_write_uint32(ff, "AID Count", &aid_count, 1)) return false;
    for(uint8_t i = 0; i < data->aid_count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "AID %u", (unsigned)i);
        if(!flipper_format_write_hex(ff, key, data->aid[i], data->aid_len[i])) return false;
    }
    return true;
}

// Text fields
static bool emv_save_text(FlipperFormat* ff, const EmvData* data) {
    for(size_t i = 0; i < COUNT_OF(k_emv_text_fields); i++) {
        const char* value = (const char*)data + k_emv_text_fields[i].offset;
        if(value[0] && !flipper_format_write_string_cstr(ff, k_emv_text_fields[i].key, value)) {
            return false;
        }
    }
    return true;
}

// Track 2 raw data
static bool emv_save_track2(FlipperFormat* ff, const EmvData* data) {
    if(data->track2_len == 0) return true;
    return flipper_format_write_hex(ff, "Track2", data->track2, data->track2_len);
}

// Transaction log
static bool emv_save_log(FlipperFormat* ff, const EmvData* data) {
    uint32_t log_count = data->log_rows;
    if(log_count == 0) return true;
    if(!flipper_format_write_uint32(ff, "Log Count", &log_count, 1)) return false;
    for(uint8_t r = 0; r < data->log_rows; r++) {
        const EmvLogRow* row = &data->log[r];
        char key_date[20], key_amt[20], key_cur[20];
        snprintf(key_date, sizeof(key_date), "Log %u Date", (unsigned)r);
        snprintf(key_amt, sizeof(key_amt), "Log %u Amount", (unsigned)r);
        snprintf(key_cur, sizeof(key_cur), "Log %u Currency", (unsigned)r);
        if(row->has_date && !flipper_format_write_hex(ff, key_date, row->date, 3)) return false;
        if(row->has_amount && !flipper_format_write_hex(ff, key_amt, row->amount, 6)) return false;
        if(row->has_currency) {
            uint32_t cur = row->currency;
            if(!flipper_format_write_uint32(ff, key_cur, &cur, 1)) return false;
        }
    }
    return true;
}

// ISO14443-4A transport, so a loaded file can emulate at transport level.
// Written with the firmware's own protocol saver - the same key layout a
// .nfc file uses - except UID, which is device-level there and so is written
// explicitly. Skipped when the device somehow holds no 4A data; such a file
// still loads, it just cannot emulate.
static bool emv_save_transport(FlipperFormat* ff, const NfcDevice* device) {
    NfcProtocol stored = nfc_device_get_protocol(device);
    if(stored != NfcProtocolIso14443_4a &&
       !nfc_protocol_has_parent(stored, NfcProtocolIso14443_4a)) {
        return true;
    }
    const Iso14443_4aData* transport =
        (const Iso14443_4aData*)nfc_device_get_data(device, NfcProtocolIso14443_4a);
    size_t uid_len = 0;
    const uint8_t* uid = iso14443_4a_get_uid(transport, &uid_len);
    if(!flipper_format_write_hex(ff, "UID", uid, uid_len)) return false;
    return iso14443_4a_save(transport, ff);
}

bool emv_save(const EmvData* data, const NfcDevice* device, const char* path) {
    if(!data || !device || !path) return false;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* ff = flipper_format_file_alloc(storage);
    bool ok = false;

    do {
        if(!flipper_format_file_open_always(ff, path)) break;
        if(!flipper_format_write_header_cstr(ff, EMV_FILE_TYPE, EMV_FILE_VERSION)) break;

        if(!emv_save_aids(ff, data)) break;
        if(!emv_save_text(ff, data)) break;
        if(!emv_save_track2(ff, data)) break;
        if(!emv_save_log(ff, data)) break;
        if(!emv_save_transport(ff, device)) break;

        ok = true;
    } while(0);

    flipper_format_file_close(ff);
    flipper_format_free(ff);
    furi_record_close(RECORD_STORAGE);
    FURI_LOG_I(TAG, "emv_save %s: %s", ok ? "ok" : "FAILED", path);
    return ok;
}

// flipper_format_read_* (read_string / read_hex / read_uint32) scan forward
// from the current cursor and, on a miss, leave the stream at EOF without
// restoring the position - unlike get_value_count / key_exist, which DO
// save and restore it. So every optional field read here must record the
// cursor first and seek back on failure; otherwise the first absent field
// strands the cursor at EOF and silently drops every field after it,
// including the ISO14443-4A transport block that gates emulation - which is
// exactly the "no transport data" / Blocked path the user hit. A card that
// withholds, say, its application label writes no "Label" line; loading that
// file then lost PAN/Expiry/UID/ATS. (The earlier "verified" card happened to
// disclose every field before any gap, so no miss stranded the cursor.)
static void emv_load_str(
    FlipperFormat* ff,
    const char* key,
    FuriString* tmp,
    char* dst,
    size_t cap) {
    size_t pos = flipper_format_tell(ff);
    if(flipper_format_read_string(ff, key, tmp)) {
        strncpy(dst, furi_string_get_cstr(tmp), cap);
        dst[cap] = '\0';
    } else {
        flipper_format_seek(ff, (int32_t)pos, FlipperFormatOffsetFromStart);
    }
    furi_string_reset(tmp);
}

static bool emv_load_hex(FlipperFormat* ff, const char* key, uint8_t* dst, uint16_t len) {
    size_t pos = flipper_format_tell(ff);
    if(flipper_format_read_hex(ff, key, dst, len)) return true;
    flipper_format_seek(ff, (int32_t)pos, FlipperFormatOffsetFromStart);
    return false;
}

static bool emv_load_u32(FlipperFormat* ff, const char* key, uint32_t* out) {
    size_t pos = flipper_format_tell(ff);
    if(flipper_format_read_uint32(ff, key, out, 1)) return true;
    flipper_format_seek(ff, (int32_t)pos, FlipperFormatOffsetFromStart);
    return false;
}

// AIDs. AID Count is mandatory-ish (always written by emv_save); a missing
// one still must not strand the cursor for what follows.
static void emv_load_aids(FlipperFormat* ff, EmvData* data) {
    uint32_t aid_count = 0;
    if(!emv_load_u32(ff, "AID Count", &aid_count)) return;
    data->aid_count = (uint8_t)(aid_count < EMV_MAX_AIDS ? aid_count : EMV_MAX_AIDS);
    for(uint8_t i = 0; i < data->aid_count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "AID %u", (unsigned)i);
        uint8_t buf[EMV_AID_MAX_LEN];
        if(!emv_load_hex(ff, key, buf, EMV_AID_MAX_LEN)) continue;
        // get_value_count restores the cursor itself, so it is safe to call
        // after a successful read; it re-seeks to the key from the start and
        // returns the value count.
        uint32_t count = 0;
        if(flipper_format_get_value_count(ff, key, &count)) {
            data->aid_len[i] = (uint8_t)(count < EMV_AID_MAX_LEN ? count : EMV_AID_MAX_LEN);
            memcpy(data->aid[i], buf, data->aid_len[i]);
        }
    }
}

// Track 2. get_value_count restores the cursor itself; the read_hex after it
// seeks from the start, so a missing Track2 is naturally safe, but the
// read_hex failure path still needs a rewind guard.
static void emv_load_track2(FlipperFormat* ff, EmvData* data) {
    uint32_t t2_count = 0;
    if(!flipper_format_get_value_count(ff, "Track2", &t2_count)) return;
    if(t2_count > EMV_TRACK2_MAX_LEN) return;
    if(emv_load_hex(ff, "Track2", data->track2, (uint16_t)t2_count)) {
        data->track2_len = (uint8_t)t2_count;
    }
}

// Transaction log. Log Count and each per-row field are all optional.
static void emv_load_log(FlipperFormat* ff, EmvData* data) {
    uint32_t log_count = 0;
    if(!emv_load_u32(ff, "Log Count", &log_count)) return;
    data->log_rows = (uint8_t)(log_count < EMV_MAX_LOG_ROWS ? log_count : EMV_MAX_LOG_ROWS);
    for(uint8_t r = 0; r < data->log_rows; r++) {
        EmvLogRow* row = &data->log[r];
        char key_date[20], key_amt[20], key_cur[20];
        snprintf(key_date, sizeof(key_date), "Log %u Date", (unsigned)r);
        snprintf(key_amt, sizeof(key_amt), "Log %u Amount", (unsigned)r);
        snprintf(key_cur, sizeof(key_cur), "Log %u Currency", (unsigned)r);
        if(emv_load_hex(ff, key_date, row->date, 3)) row->has_date = true;
        if(emv_load_hex(ff, key_amt, row->amount, 6)) row->has_amount = true;
        uint32_t cur = 0;
        if(emv_load_u32(ff, key_cur, &cur)) {
            row->currency = (uint16_t)cur;
            row->has_currency = true;
        }
    }
}

// ISO14443-4A transport block (v3 files). Optional and non-fatal: v2 files
// predate it, and a malformed block must not cost the financial fields, so
// any failure just leaves has_transport clear. The block mirrors a .nfc
// file's 4A section, so the firmware's own loader parses it; version 3 >
// NFC_LSB_ATQA_FORMAT_VERSION, so its ATQA un-swap matches the MSB-first
// order iso14443_3a_save() wrote. key_exist and get_value_count both restore
// the cursor; read_hex does not, so rewind it on failure before handing off
// to iso14443_4a_load (which reads ATQA/SAK/T0... sequentially from the
// cursor the read_hex left behind).
static void emv_load_transport(
    FlipperFormat* ff,
    EmvData* data,
    NfcDevice* device,
    uint32_t version) {
    if(!flipper_format_key_exist(ff, "UID")) return;
    Iso14443_4aData* transport = iso14443_4a_alloc();
    do {
        uint32_t uid_len = 0;
        if(!flipper_format_get_value_count(ff, "UID", &uid_len)) break;
        if(uid_len == 0 || uid_len > ISO14443_3A_MAX_UID_SIZE) break;
        size_t uid_pos = flipper_format_tell(ff);
        uint8_t uid[ISO14443_3A_MAX_UID_SIZE];
        if(!flipper_format_read_hex(ff, "UID", uid, (uint16_t)uid_len)) {
            flipper_format_seek(ff, (int32_t)uid_pos, FlipperFormatOffsetFromStart);
            break;
        }
        if(!iso14443_4a_set_uid(transport, uid, uid_len)) break;
        if(!iso14443_4a_load(transport, ff, version)) break;
        nfc_device_set_data(device, NfcProtocolIso14443_4a, (const NfcDeviceData*)transport);
        data->has_transport = true;
    } while(false);
    iso14443_4a_free(transport);
}

bool emv_load(EmvData* data, NfcDevice* device, const char* path) {
    if(!data || !device || !path) return false;

    memset(data, 0, sizeof(*data));

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* ff = flipper_format_file_alloc(storage);
    bool ok = false;

    FuriString* tmp = furi_string_alloc();

    do {
        if(!flipper_format_file_open_existing(ff, path)) break;

        FuriString* filetype = furi_string_alloc();
        uint32_t version = 0;
        bool header_ok = flipper_format_read_header(ff, filetype, &version) &&
                         furi_string_equal_str(filetype, EMV_FILE_TYPE) &&
                         version >= EMV_FILE_MIN_VERSION && version <= EMV_FILE_VERSION;
        furi_string_free(filetype);
        if(!header_ok) break;

        emv_load_aids(ff, data);

        // Text fields - each restores the cursor on a miss.
        emv_load_str(ff, "Label", tmp, data->label, EMV_LABEL_MAX_LEN);
        emv_load_str(ff, "PAN", tmp, data->pan, sizeof(data->pan) - 1);
        emv_load_str(ff, "Expiry", tmp, data->expiry, sizeof(data->expiry) - 1);
        emv_load_str(ff, "Cardholder", tmp, data->name, EMV_NAME_MAX_LEN);
        emv_load_str(ff, "Service Code", tmp, data->service_code, EMV_SERVICE_CODE_LEN);
        emv_load_str(ff, "App Preferred Name", tmp, data->app_pref_name, EMV_APP_PREF_NAME_LEN);
        emv_load_str(ff, "Issuer Country", tmp, data->issuer_country, EMV_ISSUER_COUNTRY_LEN);
        emv_load_str(ff, "Card Sequence", tmp, data->card_seq_num, EMV_CARD_SEQ_NUM_LEN);

        emv_load_track2(ff, data);
        emv_load_log(ff, data);
        emv_load_transport(ff, data, device, version);

        data->ppse_ok = true;
        ok = true;
    } while(0);

    furi_string_free(tmp);
    flipper_format_file_close(ff);
    flipper_format_free(ff);
    furi_record_close(RECORD_STORAGE);
    FURI_LOG_I(TAG, "emv_load %s: %s", ok ? "ok" : "FAILED", path);
    return ok;
}
