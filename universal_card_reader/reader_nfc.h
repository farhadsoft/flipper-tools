#pragma once

#include "reader_app.h"

bool reader_protocol_emulatable(NfcProtocol p);

void reader_stop_nfc(ReaderApp* app);
void reader_start_nfc_phase(ReaderApp* app);
void reader_start_nfc_emulation(ReaderApp* app);

bool reader_is_payment_card(const ReaderApp* app);

// Handlers for ReaderEventNfcScanned / ReaderEventNfcRead. GUI thread only
// (reader_nfc_handle_scanned() starts the poller; reader_nfc_handle_read()
// stops the radio via reader_stop_all()).
void reader_nfc_handle_scanned(ReaderApp* app);
void reader_nfc_handle_read(ReaderApp* app);
