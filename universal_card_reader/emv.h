#pragma once

#include <nfc/protocols/iso14443_4a/iso14443_4a_poller.h>

#define EMV_MAX_AIDS      4
#define EMV_AID_MAX_LEN   16
#define EMV_LABEL_MAX_LEN 16
#define EMV_NAME_MAX_LEN  26
#define EMV_MAX_LOG_ROWS  5

typedef struct {
    uint8_t date[3];    // tag 9A, YYMMDD packed BCD
    uint8_t amount[6];  // tag 9F02, n12 packed BCD
    uint16_t currency;  // tag 5F2A, ISO 4217 numeric
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
    char pan[20];                       // digits only, NUL-terminated
    char expiry[6];                     // "MM/YY"
    char name[EMV_NAME_MAX_LEN + 1];    // tag 5F20

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
