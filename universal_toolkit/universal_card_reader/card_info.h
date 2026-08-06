/*
 * Card information report renderer.
 *
 * Formats everything the poller captured about a card (transport headers,
 * protocol-specific fields, memory hex dumps, EMV data, NDEF messages) into
 * a single FuriString that the TextBox view displays. Pure formatting: all
 * input data was already read from the card by the poller before this runs.
 */
#pragma once

#include <furi.h>
#include <nfc/nfc_device.h>
#include "emv.h"

// Bounds TextBox's O(n) re-layout on huge dumps (Classic 4K is ~15 KB uncapped).
// Shared with universal_card_reader.c, which reserves info_text to this size.
#define CARD_INFO_MAX 8192

// Formats the full NFC report for `device` (already filled via nfc_device_set_data)
// into `out` (reset by caller). `display_protocol` = scanner's most-derived id
// (may be a Momentum-only id >= 12; used only for names/chain). `emv` may be all-zero.
void card_info_format_nfc(
    FuriString* out,
    const NfcDevice* device,
    NfcProtocol display_protocol,
    const EmvData* emv);

void card_info_format_lf(FuriString* out, const char* protocol_name, const uint8_t* id, size_t id_len);

// Report for EMV data restored from a .emv file: no NfcDevice behind it.
void card_info_format_emv(FuriString* out, const EmvData* emv);
