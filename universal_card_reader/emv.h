#pragma once

#include <nfc/protocols/iso14443_4a/iso14443_4a_poller.h>
#include <stdbool.h>

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
} EmvData;

/**
 * Run the read-only EMV command chain against an activated ISO14443-4A card.
 * MUST be called from inside the NfcPoller callback: iso14443_4a_poller_send_block()
 * is only valid there (iso14443_4a_poller.h:44).
 * Zeroes *out before use. Returns out->ppse_ok.
 */
bool emv_read(Iso14443_4aPoller* poller, EmvData* out);

/**
 * Save EMV data to a .emv file in FlipperFormat.
 * Stores PAN, expiry, name, AIDs, track2, transaction log, and all financial fields.
 * Returns true on success.
 */
bool emv_save(const EmvData* data, const char* path);

/**
 * Load EMV data from a .emv file in FlipperFormat.
 * Returns true on success.
 */
bool emv_load(EmvData* data, const char* path);
