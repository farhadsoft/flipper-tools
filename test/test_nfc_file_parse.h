#pragma once

// Minimal HAL-free parser for FlipperFormat-style files used by .nfc/.emv/.rfid.
// Used only by the host-side Tier-1 tests in this directory.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char* key;
    char* value;
} NfcFileEntry;

typedef struct {
    NfcFileEntry* entries;
    size_t count;
    size_t capacity;
} NfcFile;

// Parse a FlipperFormat text file into key/value pairs.
// Returns true if parsing completed; the file may be empty.
// The caller must call nfc_file_free() even on failure.
bool nfc_file_parse(const char* text, NfcFile* out);

// Free all memory held by a parsed file.
void nfc_file_free(NfcFile* file);

// Look up a key. Returns NULL if absent.
const char* nfc_file_get(const NfcFile* file, const char* key);

// Set or replace a key. Returns false on allocation failure.
bool nfc_file_set(NfcFile* file, const char* key, const char* value);

// Serialize the file back to a NUL-terminated string.
// Returns false if the buffer is too small.
bool nfc_file_serialize(const NfcFile* file, char* out, size_t out_cap, size_t* out_len);

// Parse a space-separated hex value associated with `key` into `out`.
// Returns false if the key is missing or any byte is malformed.
bool nfc_file_parse_hex(
    const NfcFile* file,
    const char* key,
    uint8_t* out,
    size_t* out_len,
    size_t out_max);
