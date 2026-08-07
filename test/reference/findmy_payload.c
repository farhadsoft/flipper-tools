// Reference-only snapshot of universal_toolkit/modules/ble_findmy/findmy_payload.c.
// Not compiled by test/Makefile (which builds the real module file). See the
// provenance note in reference/findmy_payload.h.

#include "findmy_payload.h"

#include <string.h>

// OpenHaystack SECP224R1 public key (example -- user replaces with their own).
// This array is intentionally in a HAL-free domain file so the payload can be
// parsed/generated and unit-tested on a host without the Flipper SDK.
const uint8_t findmy_public_key[FINDMY_PUBKEY_LEN] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b,
};

void findmy_payload_get(const uint8_t** data, size_t* len) {
    if(data) *data = findmy_public_key;
    if(len) *len = FINDMY_PUBKEY_LEN;
}

void findmy_build_mac(const uint8_t pubkey[FINDMY_PUBKEY_LEN], uint8_t mac_out[FINDMY_MAC_LEN]) {
    // 0xC0 marks the address as static-random (top two bits set) per the
    // Bluetooth Core Spec; bytes 1..5 are copied straight from the key.
    mac_out[0] = pubkey[0] | 0xC0;
    memcpy(&mac_out[1], &pubkey[1], FINDMY_MAC_LEN - 1);
}

void findmy_build_adv(const uint8_t pubkey[FINDMY_PUBKEY_LEN], uint8_t adv_out[FINDMY_ADV_LEN]) {
    static const uint8_t header[7] = {
        0x1e, // AD length (30 bytes follow)
        0xff, // AD type: Manufacturer Specific Data
        0x4c, 0x00, // Company ID: Apple, little-endian
        0x12, 0x19, // Offline Finding type (0x12), length (0x19 = 25)
        0x00, // State
    };
    memcpy(adv_out, header, sizeof(header));
    // 22 bytes of key data (bytes 6..27) fill the bulk of the payload.
    memcpy(&adv_out[7], &pubkey[6], 22);
    // The top 2 bits of key[0] that didn't fit in the address/payload above.
    adv_out[29] = pubkey[0] >> 6;
    adv_out[30] = 0x00; // Hint byte
}

static int hex_nibble(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool findmy_parse_hex_key(const char* hex, uint8_t out[FINDMY_PUBKEY_LEN]) {
    if(!hex || strlen(hex) != FINDMY_PUBKEY_LEN * 2) return false;

    uint8_t parsed[FINDMY_PUBKEY_LEN];
    for(size_t i = 0; i < FINDMY_PUBKEY_LEN; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if(hi < 0 || lo < 0) return false;
        parsed[i] = (uint8_t)((hi << 4) | lo);
    }
    memcpy(out, parsed, FINDMY_PUBKEY_LEN);
    return true;
}
