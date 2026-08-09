#include "mfc_key_recovery.h"

#include <furi.h>

#define TAG "UniCardReader"

// Recovery poller callback. Runs on the NFC worker thread. We deliberately
// never touch MfClassicPollerEventDataKeyRequest because its layout differs
// between official and Momentum firmware (fork ABI drift); the standard dict
// attack mode uses the known keys from the supplied MfClassicData and
// performs nested authentication internally.
static NfcCommand mfc_recover_callback(NfcGenericEvent event, void* context) {
    MfClassicData* data = context;
    MfClassicPollerEvent* e = event.event_data;

    switch(e->type) {
    case MfClassicPollerEventTypeRequestMode:
        e->data->poller_mode.mode = MfClassicPollerModeDictAttackStandard;
        e->data->poller_mode.data = data;
        break;
    case MfClassicPollerEventTypeSuccess:
    case MfClassicPollerEventTypeFail:
        return NfcCommandStop;
    default:
        break;
    }

    return NfcCommandContinue;
}

bool mfc_recover_keys(Nfc* nfc, const uint8_t* uid, uint8_t uid_len, MfClassicData* data) {
    UNUSED(uid);
    UNUSED(uid_len);

    if(!nfc || !data) return false;

    NfcPoller* poller = nfc_poller_alloc(nfc, NfcProtocolMfClassic);
    if(!poller) {
        FURI_LOG_E(TAG, "MFC key recovery: failed to allocate poller");
        return false;
    }

    FURI_LOG_I(TAG, "MFC key recovery: starting dict attack");
    nfc_poller_start(poller, mfc_recover_callback, data);
    nfc_poller_stop(poller);

    const MfClassicData* recovered = nfc_poller_get_data(poller);
    if(recovered) {
        mf_classic_copy(data, recovered);
    }

    nfc_poller_free(poller);

    bool fully_read = mf_classic_is_card_read(data);
    FURI_LOG_I(
        TAG,
        "MFC key recovery: %s",
        fully_read ? "all sectors readable" : "some sectors still locked");
    return fully_read;
}
