#pragma once

#include <stddef.h>
#include <stdint.h>

// OpenHaystack SECP224R1 public key length, in bytes.
// The Find My network scans for advertisements carrying these 28 bytes of
// compressed public-key data (preceded by a 2-byte manufacturer-specific
// header that the BLE stack normally prepends automatically).
#define FINDMY_KEY_LEN (28)

// Replace this key with your own OpenHaystack public key. It must be exactly
// FINDMY_KEY_LEN bytes. The key is exposed read-only; callers copy it into
// their own advertisement buffer.
extern const uint8_t findmy_public_key[FINDMY_KEY_LEN];

// Writes a pointer to the public-key bytes and its length.
// The pointer is valid for the lifetime of the program.
void findmy_payload_get(const uint8_t** data, size_t* len);
