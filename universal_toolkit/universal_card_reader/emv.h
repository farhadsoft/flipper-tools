#pragma once

#include <nfc/protocols/iso14443_4a/iso14443_4a_poller.h>
#include <nfc/nfc_device.h>
#include <stdbool.h>
#include "emv_emulate.h"

#define EMV_MAX_AIDS      4
#define EMV_AID_MAX_LEN   16
#define EMV_LABEL_MAX_LEN 16
#define EMV_NAME_MAX_LEN  26
#define EMV_MAX_LOG_ROWS  10
#define EMV_SERVICE_CODE_LEN 3
#define EMV_TRACK2_MAX_LEN  38
#define EMV_APP_PREF_NAME_LEN 16
#define EMV_ISSUER_COUNTRY_LEN 3
#define EMV_CARD_SEQ_NUM_LEN  3

typedef struct {
    uint8_t date[3];    // tag 9A, YYMMDD packed BCD
    uint8_t amount[6];  // tag 9F02, n12 packed BCD
    uint16_t currency;  // tag 5F2A, ISO 4217 numeric
    uint8_t country[2]; // tag 9F1A, terminal country code (for log)
    bool has_date;
    bool has_amount;
    bool has_currency;
} EmvLogRow;

typedef struct {
    uint8_t aid[EMV_MAX_AIDS][EMV_AID_MAX_LEN];
    uint8_t aid_len[EMV_MAX_AIDS];
    uint8_t aid_count;
    uint8_t aid_selected_idx;

    char label[EMV_LABEL_MAX_LEN + 1];  // tag 50, else 9F12
    char pan[20];                       // tag 5A, digits only, NUL-terminated
    char expiry[6];                     // tag 5F24, "MM/YY"
    char name[EMV_NAME_MAX_LEN + 1];    // tag 5F20, cardholder name
    char service_code[EMV_SERVICE_CODE_LEN + 1]; // tag 5F30, 3 digits
    char app_pref_name[EMV_APP_PREF_NAME_LEN + 1]; // tag 9F12
    char issuer_country[EMV_ISSUER_COUNTRY_LEN + 1]; // tag 5F28, ISO 3166
    char card_seq_num[EMV_CARD_SEQ_NUM_LEN + 1]; // tag 5F34

    uint8_t track2[EMV_TRACK2_MAX_LEN]; // tag 57, raw Track 2 equivalent data
    uint8_t track2_len;

    uint8_t log_sfi;    // from tag 9F4D byte 0
    uint8_t log_count;  // from tag 9F4D byte 1
    EmvLogRow log[EMV_MAX_LOG_ROWS];
    uint8_t log_rows;   // rows actually decoded

    bool ppse_ok;       // SELECT PPSE returned 9000 and yielded >= 1 AID
    bool aid_selected;  // SELECT AID returned 9000
    bool gpo_ok;        // GET PROCESSING OPTIONS returned 9000

    // Set by emv_load() when the file carried a valid ISO14443-4A transport
    // block (v3): the UID/ATQA/SAK/ATS were set into the NfcDevice passed to
    // emv_load(), so the card can be emulated at transport level. Always
    // false for a live read (transport lives only in the NfcDevice then) and
    // for v2 files, which predate the transport block.
    bool has_transport;

    // Raw APDU responses captured by emv_read() so a bank card can be
    // emulated at the EMV application layer, not just at ISO14443-4A
    // transport level (v4 files). ~6.4 KB: EmvData lives only inside the
    // heap-allocated ReaderApp and is only ever passed by pointer, never
    // placed on the stack.
    EmvReplay replay;
    // replay.adf was captured -- the minimum an application-layer exchange
    // needs before answering SELECT AID makes any sense.
    bool has_replay;
} EmvData;

/**
 * Run the read-only EMV command chain against an activated ISO14443-4A card.
 * MUST be called from inside the NfcPoller callback: iso14443_4a_poller_send_block()
 * is only valid there (iso14443_4a_poller.h:44).
 * Zeroes *out before use. Returns out->aid_selected.
 */
bool emv_read(Iso14443_4aPoller* poller, EmvData* out);

/**
 * Save EMV data to a .emv file in FlipperFormat (version 4).
 * Stores PAN, expiry, name, AIDs, track2, transaction log, and all financial
 * fields. When `device` holds ISO14443-4A data (always true for a card
 * reader_is_payment_card() accepts), the transport (UID/ATQA/SAK/ATS) is
 * appended with the firmware's own iso14443_4a_save(), i.e. the same key
 * layout a .nfc file uses, so a loaded file can emulate at transport level.
 * v4 also persists the raw SELECT/GPO/READ RECORD responses emv_read()
 * captured, as "Replay *" keys, so a loaded file can emulate at the EMV
 * application layer too.
 * Returns true on success.
 */
bool emv_save(const EmvData* data, const NfcDevice* device, const char* path);

/**
 * Load EMV data from a .emv file in FlipperFormat.
 * Accepts v2 (financial fields only), v3 (plus the ISO14443-4A transport
 * block) and v4 (plus the "Replay *" application-layer capture). When the
 * transport block is present and parses, it is set into `device` at protocol
 * ISO14443-4A and out->has_transport is set; the caller may then emulate
 * exactly like a live ISO14443-4A read. out->has_replay is set when a replay
 * capture was loaded, which is what arms application-layer emulation. A
 * missing or malformed transport/replay block is not fatal: the financial
 * fields still load and the corresponding flag stays false.
 * Returns true on success.
 */
bool emv_load(EmvData* data, NfcDevice* device, const char* path);
