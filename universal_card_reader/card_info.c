#include "card_info.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <nfc/protocols/nfc_protocol.h>
#include <nfc/protocols/iso14443_3a/iso14443_3a.h>
#include <nfc/protocols/iso14443_4a/iso14443_4a.h>
#include <nfc/protocols/iso15693_3/iso15693_3.h>
#include <nfc/protocols/felica/felica.h>
#include <nfc/protocols/mf_ultralight/mf_ultralight.h>
#include <nfc/protocols/mf_classic/mf_classic.h>

// Bounds TextBox's O(n) re-layout on huge dumps (Classic 4K is ~15 KB uncapped).
#define CARD_INFO_MAX 8192

/* ----------------------------- helpers ------------------------------ */

// Set once the output hit CARD_INFO_MAX; rendering is GUI-thread-only, so a
// file-scope flag is safe and keeps every helper below size-agnostic.
static bool out_truncated;

// furi_string_vcat_printf is not in the linkable API, so format through a
// stack buffer instead.
static void out_addf(FuriString* out, const char* fmt, ...) {
    if(out_truncated) return;
    if(furi_string_size(out) >= CARD_INFO_MAX) {
        furi_string_cat_str(out, "\n[truncated]");
        out_truncated = true;
        return;
    }
    char buf[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    furi_string_cat_str(out, buf);
}

// "AA BB CC" space-separated.
static void out_hex(FuriString* out, const uint8_t* data, size_t len) {
    for(size_t i = 0; i < len; i++) {
        out_addf(out, i ? " %02X" : "%02X", (unsigned)data[i]);
    }
}

/*
 * nfc_device_get_data() crashes when the requested layer is not part of the
 * device's protocol hierarchy, so every section is guarded by this first.
 * nfc_device_get_protocol() returns the polled protocol, which this app only
 * ever sets to a compile-time id 0..11 (see reader_poll_protocol()), and every
 * `layer` we ask about is a compile-time id 0..11 too — valid enum values on
 * official and Momentum alike, so the firmware-side furi_check in
 * nfc_protocol_has_parent() cannot trip.
 */
static bool dev_has(const NfcDevice* d, NfcProtocol layer) {
    NfcProtocol p = nfc_device_get_protocol(d);
    return p == layer || nfc_protocol_has_parent(p, layer);
}

/* --------------------------- EMV formatting -------------------------- */

// Tag 9A, YYMMDD packed BCD. Each nibble is already a decimal digit, so
// printing the bytes in hex reproduces the decimal date; displayed DD/MM/YY.
static void format_emv_date(const uint8_t* bcd, char* out, size_t cap) {
    snprintf(out, cap, "%02X/%02X/%02X", (unsigned)bcd[2], (unsigned)bcd[1], (unsigned)bcd[0]);
}

// Tag 9F02, 6 packed-BCD bytes (12 digits, n12). Strips leading zeros down to
// a minimum of 3 digits, then inserts '.' before the last two (minor units):
// "000000000100" -> "1.00", "000000001234" -> "12.34".
static void format_emv_amount(const uint8_t* bcd, char* out, size_t cap) {
    char digits[13];
    for(size_t i = 0; i < 6; i++) {
        digits[i * 2] = (char)('0' + (bcd[i] >> 4));
        digits[i * 2 + 1] = (char)('0' + (bcd[i] & 0x0F));
    }
    digits[12] = '\0';

    size_t start = 0;
    while(start < 9 && digits[start] == '0') start++; // keep >= 3 digits
    size_t len = 12 - start;
    const char* d = digits + start;

    snprintf(out, cap, "%.*s.%.*s", (int)(len - 2), d, 2, d + len - 2);
}

// Tag 5F2A, ISO 4217 numeric. Unknown codes print as the raw number.
static const char* format_emv_currency(uint16_t code, char* fallback, size_t fallback_cap) {
    static const struct {
        uint16_t code;
        const char* name;
    } table[] = {
        {978, "EUR"},
        {826, "GBP"},
        {840, "USD"},
        {752, "SEK"},
        {578, "NOK"},
        {208, "DKK"},
        {985, "PLN"},
        {203, "CZK"},
        {348, "HUF"},
        {756, "CHF"},
    };
    for(size_t i = 0; i < COUNT_OF(table); i++) {
        if(table[i].code == code) return table[i].name;
    }
    snprintf(fallback, fallback_cap, "%u", (unsigned)code);
    return fallback;
}

/* ------------------------------- NDEF -------------------------------- */

// NFC Forum RTD URI prefix table (36 entries, index 0 = no prefix).
static const char* const ndef_uri_prefixes[36] = {
    "",
    "http://www.",
    "https://www.",
    "http://",
    "https://",
    "tel:",
    "mailto:",
    "ftp://anonymous:anonymous@",
    "ftp://ftp.",
    "ftps://",
    "sftp://",
    "smb://",
    "nfs://",
    "ftp://",
    "dav://",
    "news:",
    "telnet://",
    "imap:",
    "rtsp://",
    "urn:",
    "pop:",
    "sip:",
    "sips:",
    "tftp:",
    "btspp://",
    "btl2cap://",
    "btgoep://",
    "tcpobex://",
    "irdaobex://",
    "file://",
    "urn:epc:id:",
    "urn:epc:tag:",
    "urn:epc:pat:",
    "urn:epc:raw:",
    "urn:epc:",
    "urn:nfc:",
};

// Printable ASCII only, capped at 96 chars + "...".
static void ndef_printable(FuriString* out, const uint8_t* data, size_t len) {
    char tmp[96 + 4];
    size_t n = 0;
    bool more = false;
    for(size_t i = 0; i < len; i++) {
        char c = (char)data[i];
        if(c < 0x20 || c > 0x7E) continue;
        if(n < 96) {
            tmp[n++] = c;
        } else {
            more = true;
        }
    }
    if(more) {
        memcpy(tmp + n, "...", 3);
        n += 3;
    }
    tmp[n] = '\0';
    out_addf(out, "%s", tmp);
}

// Parses one NDEF message: up to 4 records, only well-known (TNF 0x01) URI
// and Text records are decoded. Silent on malformed data.
static void ndef_render_message(FuriString* out, const uint8_t* msg, size_t msg_len) {
    out_addf(out, "\n[NDEF]\n");
    size_t pos = 0;
    uint8_t rec = 0;
    bool me = false;
    while(!me && pos < msg_len && rec < 4) {
        uint8_t flags = msg[pos++];
        me = flags & 0x40;
        bool sr = flags & 0x08;
        bool il = flags & 0x04;
        uint8_t tnf = flags & 0x07;

        if(pos >= msg_len) break;
        uint8_t type_len = msg[pos++];

        uint32_t payload_len;
        if(sr) {
            if(pos >= msg_len) break;
            payload_len = msg[pos++];
        } else {
            if(pos + 4 > msg_len) break;
            payload_len = ((uint32_t)msg[pos] << 24) | ((uint32_t)msg[pos + 1] << 16) |
                          ((uint32_t)msg[pos + 2] << 8) | (uint32_t)msg[pos + 3];
            pos += 4;
        }

        uint8_t id_len = 0;
        if(il) {
            if(pos >= msg_len) break;
            id_len = msg[pos++];
        }
        if(pos + type_len + id_len + payload_len > msg_len) break;

        const uint8_t* type = msg + pos;
        pos += (size_t)type_len + id_len;
        const uint8_t* payload = msg + pos;
        pos += payload_len;

        if(tnf == 0x01 && type_len == 1 && type[0] == 'U' && payload_len >= 1) {
            const char* prefix = "";
            if(payload[0] < COUNT_OF(ndef_uri_prefixes)) {
                prefix = ndef_uri_prefixes[payload[0]];
            }
            out_addf(out, "URI: %s", prefix);
            ndef_printable(out, payload + 1, payload_len - 1);
            out_addf(out, "\n");
        } else if(tnf == 0x01 && type_len == 1 && type[0] == 'T' && payload_len >= 1) {
            uint8_t status = payload[0];
            size_t lang_len = status & 0x3F;
            if(status & 0x80) {
                size_t hex_len = payload_len - 1 < 32 ? payload_len - 1 : 32;
                out_addf(out, "Text (UTF-16): ");
                out_hex(out, payload + 1, hex_len);
                out_addf(out, "\n");
            } else {
                size_t off = 1 + lang_len;
                if(off > payload_len) off = payload_len;
                out_addf(out, "Text: ");
                ndef_printable(out, payload + off, payload_len - off);
                out_addf(out, "\n");
            }
        } else {
            out_addf(out, "Record: type ");
            ndef_printable(out, type, type_len);
            out_addf(out, "\n");
        }
        rec++;
    }
    if(rec == 4 && !me) out_addf(out, "(...more)\n");
}

// Best-effort NDEF extraction from already-read MfUltralight pages: the TLV
// area starts at page 4. No card I/O happens here.
static void ndef_render_ultralight(FuriString* out, const MfUltralightData* d) {
    uint16_t pages = d->pages_read < 64 ? d->pages_read : 64;
    if(pages <= 4) return;

    uint8_t buf[240]; // 60 pages x 4 bytes
    size_t len = (size_t)(pages - 4) * MF_ULTRALIGHT_PAGE_SIZE;
    for(uint16_t i = 4; i < pages; i++) {
        memcpy(buf + (size_t)(i - 4) * MF_ULTRALIGHT_PAGE_SIZE, d->page[i].data, MF_ULTRALIGHT_PAGE_SIZE);
    }

    // TLV walk: skip 0x00, stop at 0xFE, parse only the first 0x03 (NDEF message).
    size_t pos = 0;
    while(pos < len) {
        uint8_t t = buf[pos++];
        if(t == 0x00) continue;
        if(t == 0xFE) break;
        if(pos >= len) break;
        size_t tlv_len;
        if(buf[pos] == 0xFF) {
            if(pos + 2 >= len) break;
            tlv_len = ((size_t)buf[pos + 1] << 8) | (size_t)buf[pos + 2];
            pos += 3;
        } else {
            tlv_len = buf[pos];
            pos += 1;
        }
        if(pos + tlv_len > len) break;
        if(t == 0x03) {
            ndef_render_message(out, buf + pos, tlv_len);
            break;
        }
        pos += tlv_len;
    }
}

/* --------------------------- report sections ------------------------- */

static void card_info_iso14443_3a(FuriString* out, const NfcDevice* device) {
    if(!dev_has(device, NfcProtocolIso14443_3a)) return;
    const Iso14443_3aData* d =
        (const Iso14443_3aData*)nfc_device_get_data(device, NfcProtocolIso14443_3a);
    uint8_t atqa[2];
    iso14443_3a_get_atqa(d, atqa);
    out_addf(out, "\n[ISO14443-3A]\n");
    out_addf(out, "ATQA: ");
    out_hex(out, atqa, sizeof(atqa));
    out_addf(out, "\n");
    out_addf(out, "SAK: 0x%02X\n", (unsigned)iso14443_3a_get_sak(d));
}

static void card_info_iso14443_4a(FuriString* out, const NfcDevice* device) {
    if(!dev_has(device, NfcProtocolIso14443_4a)) return;
    const Iso14443_4aData* d =
        (const Iso14443_4aData*)nfc_device_get_data(device, NfcProtocolIso14443_4a);
    out_addf(out, "\n[ISO14443-4A]\n");
    out_addf(out, "ATS: TL=%02X T0=%02X", (unsigned)d->ats_data.tl, (unsigned)d->ats_data.t0);
    if(d->ats_data.ta_1) out_addf(out, " TA1=%02X", (unsigned)d->ats_data.ta_1);
    if(d->ats_data.tb_1) out_addf(out, " TB1=%02X", (unsigned)d->ats_data.tb_1);
    if(d->ats_data.tc_1) out_addf(out, " TC1=%02X", (unsigned)d->ats_data.tc_1);
    out_addf(out, "\n");
    uint32_t count = 0;
    const uint8_t* hist = iso14443_4a_get_historical_bytes(d, &count);
    if(count > 0) {
        out_addf(out, "Hist: ");
        out_hex(out, hist, count);
        out_addf(out, "\n");
    }
}

static void card_info_iso15693_3(FuriString* out, const NfcDevice* device) {
    if(!dev_has(device, NfcProtocolIso15693_3)) return;
    const Iso15693_3Data* d =
        (const Iso15693_3Data*)nfc_device_get_data(device, NfcProtocolIso15693_3);
    out_addf(out, "\n[ISO15693-3]\n");
    out_addf(out, "Mfg: 0x%02X\n", (unsigned)iso15693_3_get_manufacturer_id(d));
    if(d->system_info.flags & ISO15693_3_SYSINFO_FLAG_DSFID) {
        out_addf(out, "DSFID: 0x%02X\n", (unsigned)d->system_info.dsfid);
    }
    if(d->system_info.flags & ISO15693_3_SYSINFO_FLAG_AFI) {
        out_addf(out, "AFI: 0x%02X\n", (unsigned)d->system_info.afi);
    }
    if(d->system_info.flags & ISO15693_3_SYSINFO_FLAG_IC_REF) {
        out_addf(out, "IC ref: 0x%02X\n", (unsigned)d->system_info.ic_ref);
    }
    uint16_t blocks = iso15693_3_get_block_count(d);
    uint8_t block_size = iso15693_3_get_block_size(d);
    out_addf(out, "Blocks: %u x %u bytes\n", (unsigned)blocks, (unsigned)block_size);
    if(blocks == 0) return; // block_data SimpleArray is uninitialised then
    uint16_t limit = blocks > 255 ? 255 : blocks; // the block getters take uint8_t
    for(uint16_t i = 0; i < limit; i++) {
        out_addf(out, "BLK %02u: ", (unsigned)i);
        out_hex(out, iso15693_3_get_block_data(d, (uint8_t)i), block_size);
        if(iso15693_3_is_block_locked(d, (uint8_t)i)) out_addf(out, " *");
        out_addf(out, "\n");
    }
}

static void card_info_felica(FuriString* out, const NfcDevice* device) {
    if(!dev_has(device, NfcProtocolFelica)) return;
    const FelicaData* d = (const FelicaData*)nfc_device_get_data(device, NfcProtocolFelica);
    out_addf(out, "\n[FeliCa]\n");
    out_addf(out, "IDm: ");
    out_hex(out, d->idm.data, FELICA_IDM_SIZE);
    out_addf(out, "\n");
    out_addf(out, "PMm: ");
    out_hex(out, d->pmm.data, FELICA_PMM_SIZE);
    out_addf(out, "\n");
    out_addf(out, "Blocks: %u/%u read\n", (unsigned)d->blocks_read, (unsigned)d->blocks_total);
}

// Ids 0..11 are identical on official and Momentum; anything else prints raw.
static const char* const mf_ul_type_names[] = {
    "Ultralight",
    "NTAG203",
    "Ultralight-C",
    "UL11",
    "UL21",
    "NTAG213",
    "NTAG215",
    "NTAG216",
    "NTAG I2C 1K",
    "NTAG I2C 2K",
    "NTAG I2C+ 1K",
    "NTAG I2C+ 2K",
};

static void card_info_mf_ultralight(FuriString* out, const NfcDevice* device) {
    if(!dev_has(device, NfcProtocolMfUltralight)) return;
    const MfUltralightData* d =
        (const MfUltralightData*)nfc_device_get_data(device, NfcProtocolMfUltralight);
    out_addf(out, "\n[Mifare Ultralight]\n");
    if((unsigned)d->type < COUNT_OF(mf_ul_type_names)) {
        out_addf(out, "Type: %s\n", mf_ul_type_names[d->type]);
    } else {
        out_addf(out, "Type: %u\n", (unsigned)d->type);
    }
    out_addf(out, "Pages: %u/%u\n", (unsigned)d->pages_read, (unsigned)d->pages_total);
    uint16_t limit =
        d->pages_read < MF_ULTRALIGHT_MAX_PAGE_NUM ? d->pages_read : MF_ULTRALIGHT_MAX_PAGE_NUM;
    for(uint16_t i = 0; i < limit; i++) {
        out_addf(out, "P %02u: ", (unsigned)i);
        out_hex(out, d->page[i].data, MF_ULTRALIGHT_PAGE_SIZE);
        out_addf(out, "\n");
    }
    ndef_render_ultralight(out, d);
}

// Ids 0..2 are identical on official and Momentum.
static const char* const mf_classic_type_names[] = {"Mini", "1K", "4K"};

static void card_info_mf_classic(FuriString* out, const NfcDevice* device) {
    if(!dev_has(device, NfcProtocolMfClassic)) return;
    const MfClassicData* d =
        (const MfClassicData*)nfc_device_get_data(device, NfcProtocolMfClassic);
    out_addf(out, "\n[Mifare Classic]\n");
    if((unsigned)d->type < COUNT_OF(mf_classic_type_names)) {
        out_addf(out, "Type: %s\n", mf_classic_type_names[d->type]);
    } else {
        out_addf(out, "Type: %u\n", (unsigned)d->type);
    }

    uint8_t sectors = mf_classic_get_total_sectors_num(d->type);
    uint8_t sectors_read = 0;
    for(uint8_t s = 0; s < sectors; s++) {
        if(mf_classic_is_sector_read(d, s)) sectors_read++;
    }
    out_addf(out, "Sectors: %u/%u read\n", (unsigned)sectors_read, (unsigned)sectors);

    // Sector map, 8 sectors per line: '+' all blocks read, '~' partial, '.' nothing.
    for(uint8_t base = 0; base < sectors; base += 8) {
        uint8_t last = base + 7 < sectors ? base + 7 : sectors - 1;
        out_addf(out, "Sectors %02u-%02u: ", (unsigned)base, (unsigned)last);
        for(uint8_t s = base; s <= last; s++) {
            uint8_t first = mf_classic_get_first_block_num_of_sector(s);
            uint8_t count = mf_classic_get_blocks_num_in_sector(s);
            uint8_t read = 0;
            for(uint8_t b = 0; b < count; b++) {
                if(mf_classic_is_block_read(d, first + b)) read++;
            }
            out_addf(out, "%c", read == 0 ? '.' : (read == count ? '+' : '~'));
        }
        out_addf(out, "\n");
    }

    uint16_t total_blocks = mf_classic_get_total_block_num(d->type);
    if(total_blocks > 255) total_blocks = 255; // mf_classic_is_block_read takes uint8_t
    for(uint16_t b = 0; b < total_blocks; b++) {
        if(mf_classic_is_block_read(d, (uint8_t)b)) {
            out_addf(out, "B %03u: ", (unsigned)b);
            out_hex(out, d->block[b].data, MF_CLASSIC_BLOCK_SIZE);
            out_addf(out, "\n");
        }
    }
}

static void card_info_emv(FuriString* out, const NfcDevice* device, const EmvData* emv) {
    if(!emv->aid_selected && !emv->ppse_ok) {
        if(device && dev_has(device, NfcProtocolIso14443_4a)) {
            out_addf(out, "No EMV app on card\n");
        }
        return;
    }

    out_addf(out, "\n[EMV / Bank card]\n");
    for(uint8_t i = 0; i < emv->aid_count; i++) {
        out_addf(out, i == 0 ? "AID: " : "AID+: ");
        out_hex(out, emv->aid[i], emv->aid_len[i]);
        out_addf(out, "\n");
    }
    if(emv->label[0]) out_addf(out, "App: %s\n", emv->label);

    if(emv->pan[0]) {
        out_addf(out, "PAN: %s\n", emv->pan);
    } else {
        out_addf(out, "PAN: not available over contactless\n");
    }

    if(emv->expiry[0]) {
        out_addf(out, "Expiry: %s\n", emv->expiry);
    } else {
        out_addf(out, "Expiry: not disclosed\n");
    }

    if(emv->name[0]) {
        out_addf(out, "Name: %s\n", emv->name);
    } else {
        out_addf(out, "Name: not disclosed\n");
    }

    if(emv->service_code[0]) {
        out_addf(out, "Service Code: %s\n", emv->service_code);
    }
    if(emv->app_pref_name[0]) {
        out_addf(out, "Pref Name: %s\n", emv->app_pref_name);
    }
    if(emv->issuer_country[0]) {
        out_addf(out, "Country: %s\n", emv->issuer_country);
    }
    if(emv->card_seq_num[0]) {
        out_addf(out, "Seq #: %s\n", emv->card_seq_num);
    }
    if(emv->track2_len > 0) {
        out_addf(out, "Track2: ");
        out_hex(out, emv->track2, emv->track2_len);
        out_addf(out, "\n");
    }

    if(emv->log_count) {
        out_addf(out, "Txn log: %u records\n", (unsigned)emv->log_count);
        for(uint8_t r = 0; r < emv->log_rows; r++) {
            const EmvLogRow* row = &emv->log[r];
            char date_str[12];
            char amount_str[16];
            char cur_fallback[8];
            const char* cur_str = "--";
            if(row->has_date) {
                format_emv_date(row->date, date_str, sizeof(date_str));
            } else {
                snprintf(date_str, sizeof(date_str), "--/--/--");
            }
            if(row->has_amount) {
                format_emv_amount(row->amount, amount_str, sizeof(amount_str));
            } else {
                snprintf(amount_str, sizeof(amount_str), "--");
            }
            if(row->has_currency) {
                cur_str = format_emv_currency(row->currency, cur_fallback, sizeof(cur_fallback));
            }
            out_addf(out, "%s %s %s\n", date_str, amount_str, cur_str);
        }
        if(emv->log_rows == 0) out_addf(out, "(log format not given)\n");
    }
}

/* ------------------------------ public API --------------------------- */

void card_info_format_nfc(
    FuriString* out,
    const NfcDevice* device,
    NfcProtocol display_protocol,
    const EmvData* emv) {
    out_truncated = false;

    out_addf(out, "Band: 13.56 MHz\n");
    out_addf(out, "Type: %s\n", nfc_device_get_protocol_name(display_protocol));

    // Protocol chain, most-derived last. The walk never feeds NfcProtocolInvalid
    // back into nfc_protocol_get_parent(): display_protocol comes from the
    // scanner (always a real id) and every derived protocol's chain ends at one
    // of the five transports below.
    NfcProtocol chain[8];
    size_t n = 0;
    NfcProtocol p = display_protocol;
    while(n < COUNT_OF(chain)) {
        chain[n++] = p;
        if(p == NfcProtocolIso14443_3a || p == NfcProtocolIso14443_3b ||
           p == NfcProtocolIso15693_3 || p == NfcProtocolFelica || p == NfcProtocolSt25tb) {
            break;
        }
        p = nfc_protocol_get_parent(p);
    }
    if(n > 1) {
        out_addf(out, "Chain: ");
        for(size_t i = n; i-- > 0;) {
            out_addf(out, "%s%s", nfc_device_get_protocol_name(chain[i]), i ? " > " : "");
        }
        out_addf(out, "\n");
    }

    size_t uid_len = 0;
    const uint8_t* uid = nfc_device_get_uid(device, &uid_len);
    out_addf(out, "UID: ");
    out_hex(out, uid, uid_len);
    out_addf(out, "\n");

    card_info_iso14443_3a(out, device);
    card_info_iso14443_4a(out, device);
    card_info_iso15693_3(out, device);
    card_info_felica(out, device);
    card_info_mf_ultralight(out, device);
    card_info_mf_classic(out, device);
    card_info_emv(out, device, emv);
}

void card_info_format_lf(FuriString* out, const char* protocol_name, const uint8_t* id, size_t id_len) {
    out_truncated = false;
    out_addf(out, "Band: 125 kHz\n");
    out_addf(out, "Type: %s\n", protocol_name);
    out_addf(out, "ID: ");
    out_hex(out, id, id_len);
    out_addf(out, "\n");
}

// Report for EMV data restored from a .emv file: there is no NfcDevice behind
// it, so only the EMV block is rendered. emv_load() sets ppse_ok, so
// card_info_emv() never reaches its device-dependent branch.
void card_info_format_emv(FuriString* out, const EmvData* emv) {
    out_truncated = false;
    out_addf(out, "Band: 13.56 MHz\n");
    out_addf(out, "Type: EMV (from file)\n");
    card_info_emv(out, NULL, emv);
}
