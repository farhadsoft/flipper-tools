// Reference-only snapshot of universal_toolkit/modules/ble_findmy/findmy_payload.h.
// Not compiled by test/Makefile (which builds the real module file); kept
// here so the domain API this test harness targets has a standalone,
// version-controlled record independent of the module tree.
//
// Provenance: the original plan for this harness called for placing a
// user-delivered reference implementation here verbatim. That delivered
// content was not recoverable from the workspace, git history, or any
// session artifact (see the "Test harness provenance" note in ../../CLAUDE.md
// under BLE Find My). This file was instead implemented directly against the
// primary source the plan itself names as the correctness standard --
// seemoo-lab/openhaystack's ESP32 reference firmware -- and is identical to
// the shipped module header.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// OpenHaystack SECP224R1 public key length, in bytes.
#define FINDMY_PUBKEY_LEN (28)

// BLE static-random device address length, in bytes.
#define FINDMY_MAC_LEN (6)

// Full Apple Find My "offline finding" advertisement length, in bytes:
// 1 (AD length) + 1 (AD type 0xFF) + 2 (company ID) + 2 (type+len) +
// 1 (state) + 22 (key bytes 6..27) + 1 (key[0] top 2 bits) + 1 (hint).
#define FINDMY_ADV_LEN (31)

// Replace this key with your own OpenHaystack public key. It must be exactly
// FINDMY_PUBKEY_LEN bytes. The key is exposed read-only; callers derive the
// MAC and advertisement from it via findmy_build_mac() / findmy_build_adv().
extern const uint8_t findmy_public_key[FINDMY_PUBKEY_LEN];

// Writes a pointer to the public-key bytes and its length.
// The pointer is valid for the lifetime of the program.
void findmy_payload_get(const uint8_t** data, size_t* len);

// Derives the BLE static-random device address from a Find My public key:
// mac_out[0] = pubkey[0] | 0xC0 (marks it static-random per the Bluetooth
// spec), mac_out[1..5] = pubkey[1..5]. Matches the OpenHaystack reference
// firmware's set_addr_from_key().
void findmy_build_mac(const uint8_t pubkey[FINDMY_PUBKEY_LEN], uint8_t mac_out[FINDMY_MAC_LEN]);

// Builds the full 31-byte Apple Find My "offline finding" AD structure for a
// public key: a length-prefixed Manufacturer Specific Data (0xFF) block
// carrying Apple's company ID (0x004C), the offline-finding type/length
// (0x12, 0x19), a zero state byte, key bytes 6..27, the top 2 bits of
// key[0], and a zero hint byte. This buffer is passed directly as the raw
// advertising payload -- it needs no separate Flags AD structure. Matches
// the OpenHaystack reference firmware's set_payload_from_key().
void findmy_build_adv(const uint8_t pubkey[FINDMY_PUBKEY_LEN], uint8_t adv_out[FINDMY_ADV_LEN]);

// Parses exactly FINDMY_PUBKEY_LEN*2 (56) hex characters (case-insensitive,
// no separators, no "0x" prefix) into out[FINDMY_PUBKEY_LEN]. Returns false
// without writing to out on wrong length or any non-hex character.
bool findmy_parse_hex_key(const char* hex, uint8_t out[FINDMY_PUBKEY_LEN]);
