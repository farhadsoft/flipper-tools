#include "findmy_payload.h"

// OpenHaystack SECP224R1 public key (example — user replaces with their own).
// This array is intentionally in a HAL-free domain file so the payload can be
// parsed/generated and unit-tested on a host without the Flipper SDK.
const uint8_t findmy_public_key[FINDMY_KEY_LEN] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b,
};

void findmy_payload_get(const uint8_t** data, size_t* len) {
    if(data) *data = findmy_public_key;
    if(len) *len = FINDMY_KEY_LEN;
}
