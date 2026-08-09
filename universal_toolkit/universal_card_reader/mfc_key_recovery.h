#pragma once

// Mifare Classic key recovery for partially-read cards.
// Uses the firmware's MfClassic poller in dictionary-attack-standard mode
// to recover sector keys from known keys via nested authentication.

#include <nfc/protocols/mf_classic/mf_classic.h>
#include <nfc/protocols/mf_classic/mf_classic_poller.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Attempt to recover missing Mifare Classic sector keys.
 *
 * `nfc` must be the active Nfc instance. `data` is the partially-read card
 * state (used for known keys and updated with recovered keys/blocks).
 * `uid`/`uid_len` are informational; the function reads the UID from
 * `data` if needed.
 *
 * Returns true if the card is fully readable after recovery.
 */
bool mfc_recover_keys(Nfc* nfc, const uint8_t* uid, uint8_t uid_len, MfClassicData* data);

#ifdef __cplusplus
}
#endif
