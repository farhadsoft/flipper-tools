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

#define TAG "UniEmv"

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
        case 0x5A:
            if(out->pan[0] == '\0') emv_harvest_pan_bcd(val, val_len, out->pan, sizeof(out->pan));
            break;
        case 0x5F24:
            if(out->expiry[0] == '\0' && val_len >= 2) {
                snprintf(
                    out->expiry,
                    sizeof(out->expiry),
                    "%02X/%02X",
                    (unsigned)val[1],
                    (unsigned)val[0]);
            }
            break;
        case 0x5F20:
            if(out->name[0] == '\0') emv_copy_name(val, val_len, out->name, sizeof(out->name));
            break;
        case 0x57:
        case 0x9F6B:
            emv_harvest_track2(val, val_len, out);
            break;
        case 0x50:
            if(out->label[0] == '\0') emv_set_label(out->label, sizeof(out->label), val, val_len);
            break;
        case 0x5F30:
            // Service code, 3 digits BCD
            if(out->service_code[0] == '\0' && val_len >= 2) {
                snprintf(
                    out->service_code,
                    sizeof(out->service_code),
                    "%02X%01X",
                    (unsigned)val[0],
                    (unsigned)(val[1] >> 4));
            }
            break;
        case 0x9F12:
            if(out->app_pref_name[0] == '\0') {
                size_t n = val_len < EMV_APP_PREF_NAME_LEN ? val_len : EMV_APP_PREF_NAME_LEN;
                memcpy(out->app_pref_name, val, n);
                out->app_pref_name[n] = '\0';
            }
            break;
        case 0x5F28:
            // Issuer country code, ISO 3166, BCD 2 bytes
            if(out->issuer_country[0] == '\0' && val_len >= 2) {
                unsigned cc = (unsigned)(val[0] << 8) | val[1];
                if(cc > 999) cc = 999;
                snprintf(
                    out->issuer_country,
                    sizeof(out->issuer_country),
                    "%03u",
                    cc);
            }
            break;
        case 0x5F34:
            // Card sequence number
            if(out->card_seq_num[0] == '\0' && val_len >= 1) {
                size_t n = val_len < EMV_CARD_SEQ_NUM_LEN ? val_len : EMV_CARD_SEQ_NUM_LEN;
                for(size_t k = 0; k < n; k++) {
                    out->card_seq_num[k] = (char)('0' + ((val[k] >> 4) & 0x0F));
                }
                out->card_seq_num[n] = '\0';
            }
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

    if(ok && sw == 0x9000) {
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
    if(!ok || sw != 0x9000 || out->aid_count == 0) return false;

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

        if(!ok || sw != 0x9000) continue;

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

    if(ok && sw == 0x9000 && body_len > 0) {
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

            if(ok && sw == 0x9000) emv_harvest(body, body_len, out);
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
       sw != 0x9000) {
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

        if(!ok || rsw != 0x9000) continue;
        emv_parse_log_record(dol, dol_len, rbody, rbody_len, &out->log[out->log_rows]);
        out->log_rows++;
    }
}

bool emv_read(Iso14443_4aPoller* poller, EmvData* out) {
    memset(out, 0, sizeof(*out));

    BitBuffer* tx = bit_buffer_alloc(256);
    BitBuffer* rx = bit_buffer_alloc(256);

    if(emv_select_ppse(poller, tx, rx, out)) {
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
        "result: PAN %s exp %s label '%s' log %u/%u",
        pan_masked,
        out->expiry[0] ? out->expiry : "(none)",
        out->label,
        (unsigned)out->log_rows,
        (unsigned)out->log_count);
    FURI_LOG_D(TAG, "unmasked PAN %s", out->pan);

    bit_buffer_free(tx);
    bit_buffer_free(rx);
    return out->ppse_ok;
}

/* -------------------------- save / load --------------------------- */

#define EMV_FILE_TYPE    "Universal EMV Card"
#define EMV_FILE_VERSION 2

bool emv_save(const EmvData* data, const char* path) {
    if(!data || !path) return false;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* ff = flipper_format_file_alloc(storage);
    bool ok = false;

    do {
        if(!flipper_format_file_open_always(ff, path)) break;
        if(!flipper_format_write_header_cstr(ff, EMV_FILE_TYPE, EMV_FILE_VERSION)) break;

        // AIDs
        uint32_t aid_count = data->aid_count;
        if(!flipper_format_write_uint32(ff, "AID Count", &aid_count, 1)) break;
        for(uint8_t i = 0; i < data->aid_count; i++) {
            char key[16];
            snprintf(key, sizeof(key), "AID %u", (unsigned)i);
            if(!flipper_format_write_hex(ff, key, data->aid[i], data->aid_len[i])) break;
        }

        // Text fields
        if(data->label[0]) {
            if(!flipper_format_write_string_cstr(ff, "Label", data->label)) break;
        }
        if(data->pan[0]) {
            if(!flipper_format_write_string_cstr(ff, "PAN", data->pan)) break;
        }
        if(data->expiry[0]) {
            if(!flipper_format_write_string_cstr(ff, "Expiry", data->expiry)) break;
        }
        if(data->name[0]) {
            if(!flipper_format_write_string_cstr(ff, "Cardholder", data->name)) break;
        }
        if(data->service_code[0]) {
            if(!flipper_format_write_string_cstr(ff, "Service Code", data->service_code)) break;
        }
        if(data->app_pref_name[0]) {
            if(!flipper_format_write_string_cstr(ff, "App Preferred Name", data->app_pref_name)) break;
        }
        if(data->issuer_country[0]) {
            if(!flipper_format_write_string_cstr(ff, "Issuer Country", data->issuer_country)) break;
        }
        if(data->card_seq_num[0]) {
            if(!flipper_format_write_string_cstr(ff, "Card Sequence", data->card_seq_num)) break;
        }

        // Track 2 raw data
        if(data->track2_len > 0) {
            if(!flipper_format_write_hex(ff, "Track2", data->track2, data->track2_len)) break;
        }

        // Transaction log
        uint32_t log_count = data->log_rows;
        if(log_count > 0) {
            if(!flipper_format_write_uint32(ff, "Log Count", &log_count, 1)) break;
            for(uint8_t r = 0; r < data->log_rows; r++) {
                const EmvLogRow* row = &data->log[r];
                char key_date[20], key_amt[20], key_cur[20];
                snprintf(key_date, sizeof(key_date), "Log %u Date", (unsigned)r);
                snprintf(key_amt, sizeof(key_amt), "Log %u Amount", (unsigned)r);
                snprintf(key_cur, sizeof(key_cur), "Log %u Currency", (unsigned)r);
                if(row->has_date) {
                    if(!flipper_format_write_hex(ff, key_date, row->date, 3)) break;
                }
                if(row->has_amount) {
                    if(!flipper_format_write_hex(ff, key_amt, row->amount, 6)) break;
                }
                if(row->has_currency) {
                    uint32_t cur = row->currency;
                    if(!flipper_format_write_uint32(ff, key_cur, &cur, 1)) break;
                }
            }
        }

        ok = true;
    } while(0);

    flipper_format_file_close(ff);
    flipper_format_free(ff);
    furi_record_close(RECORD_STORAGE);
    FURI_LOG_I(TAG, "emv_save %s: %s", ok ? "ok" : "FAILED", path);
    return ok;
}

bool emv_load(EmvData* data, const char* path) {
    if(!data || !path) return false;

    memset(data, 0, sizeof(*data));

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* ff = flipper_format_file_alloc(storage);
    bool ok = false;

    FuriString* tmp = furi_string_alloc();

    do {
        if(!flipper_format_file_open_existing(ff, path)) break;

        FuriString* filetype = furi_string_alloc();
        uint32_t version = 0;
        if(!flipper_format_read_header(ff, filetype, &version)) {
            furi_string_free(filetype);
            break;
        }
        furi_string_free(filetype);

        // AIDs
        uint32_t aid_count = 0;
        if(flipper_format_read_uint32(ff, "AID Count", &aid_count, 1)) {
            data->aid_count = (uint8_t)(aid_count < EMV_MAX_AIDS ? aid_count : EMV_MAX_AIDS);
            for(uint8_t i = 0; i < data->aid_count; i++) {
                char key[16];
                snprintf(key, sizeof(key), "AID %u", (unsigned)i);
                uint8_t buf[EMV_AID_MAX_LEN];
                if(flipper_format_read_hex(ff, key, buf, EMV_AID_MAX_LEN)) {
                    // We need the actual length - read via key_exist + count
                    uint32_t count = 0;
                    if(flipper_format_get_value_count(ff, key, &count)) {
                        data->aid_len[i] = (uint8_t)(count < EMV_AID_MAX_LEN ? count : EMV_AID_MAX_LEN);
                        memcpy(data->aid[i], buf, data->aid_len[i]);
                    }
                }
            }
        }

        // Text fields
        if(flipper_format_read_string(ff, "Label", tmp)) {
            strncpy(data->label, furi_string_get_cstr(tmp), EMV_LABEL_MAX_LEN);
            data->label[EMV_LABEL_MAX_LEN] = '\0';
        }
        furi_string_reset(tmp);
        if(flipper_format_read_string(ff, "PAN", tmp)) {
            strncpy(data->pan, furi_string_get_cstr(tmp), sizeof(data->pan) - 1);
            data->pan[sizeof(data->pan) - 1] = '\0';
        }
        furi_string_reset(tmp);
        if(flipper_format_read_string(ff, "Expiry", tmp)) {
            strncpy(data->expiry, furi_string_get_cstr(tmp), sizeof(data->expiry) - 1);
            data->expiry[sizeof(data->expiry) - 1] = '\0';
        }
        furi_string_reset(tmp);
        if(flipper_format_read_string(ff, "Cardholder", tmp)) {
            strncpy(data->name, furi_string_get_cstr(tmp), EMV_NAME_MAX_LEN);
            data->name[EMV_NAME_MAX_LEN] = '\0';
        }
        furi_string_reset(tmp);
        if(flipper_format_read_string(ff, "Service Code", tmp)) {
            strncpy(data->service_code, furi_string_get_cstr(tmp), EMV_SERVICE_CODE_LEN);
            data->service_code[EMV_SERVICE_CODE_LEN] = '\0';
        }
        furi_string_reset(tmp);
        if(flipper_format_read_string(ff, "App Preferred Name", tmp)) {
            strncpy(data->app_pref_name, furi_string_get_cstr(tmp), EMV_APP_PREF_NAME_LEN);
            data->app_pref_name[EMV_APP_PREF_NAME_LEN] = '\0';
        }
        furi_string_reset(tmp);
        if(flipper_format_read_string(ff, "Issuer Country", tmp)) {
            strncpy(data->issuer_country, furi_string_get_cstr(tmp), EMV_ISSUER_COUNTRY_LEN);
            data->issuer_country[EMV_ISSUER_COUNTRY_LEN] = '\0';
        }
        furi_string_reset(tmp);
        if(flipper_format_read_string(ff, "Card Sequence", tmp)) {
            strncpy(data->card_seq_num, furi_string_get_cstr(tmp), EMV_CARD_SEQ_NUM_LEN);
            data->card_seq_num[EMV_CARD_SEQ_NUM_LEN] = '\0';
        }

        // Track 2
        uint32_t t2_count = 0;
        if(flipper_format_get_value_count(ff, "Track2", &t2_count) && t2_count <= EMV_TRACK2_MAX_LEN) {
            if(flipper_format_read_hex(ff, "Track2", data->track2, (uint16_t)t2_count)) {
                data->track2_len = (uint8_t)t2_count;
            }
        }

        // Transaction log
        uint32_t log_count = 0;
        if(flipper_format_read_uint32(ff, "Log Count", &log_count, 1)) {
            data->log_rows = (uint8_t)(log_count < EMV_MAX_LOG_ROWS ? log_count : EMV_MAX_LOG_ROWS);
            for(uint8_t r = 0; r < data->log_rows; r++) {
                EmvLogRow* row = &data->log[r];
                char key_date[20], key_amt[20], key_cur[20];
                snprintf(key_date, sizeof(key_date), "Log %u Date", (unsigned)r);
                snprintf(key_amt, sizeof(key_amt), "Log %u Amount", (unsigned)r);
                snprintf(key_cur, sizeof(key_cur), "Log %u Currency", (unsigned)r);
                if(flipper_format_read_hex(ff, key_date, row->date, 3)) {
                    row->has_date = true;
                }
                if(flipper_format_read_hex(ff, key_amt, row->amount, 6)) {
                    row->has_amount = true;
                }
                uint32_t cur = 0;
                if(flipper_format_read_uint32(ff, key_cur, &cur, 1)) {
                    row->currency = (uint16_t)cur;
                    row->has_currency = true;
                }
            }
        }

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
