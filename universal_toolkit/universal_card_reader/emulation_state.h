#pragma once

// HAL-free struct capturing everything the emulator presents.
// Populated by the read path, consumed by the emulate path,
// and testable on host.

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EMULATION_STATE_UID_MAX  10
#define EMULATION_STATE_ATS_MAX  32

typedef struct {
    uint8_t uid[EMULATION_STATE_UID_MAX];
    uint8_t uid_len;
    uint8_t atqa[2];
    uint8_t sak;
    uint8_t ats[EMULATION_STATE_ATS_MAX];
    uint8_t ats_len;
    // Protocol-specific data is intentionally not duplicated here:
    // the firmware's NfcDeviceData is opaque and varies per protocol.
    // For host-side fidelity tests we compare the save-file bytes directly.
} EmulationState;

#ifdef __cplusplus
}
#endif
