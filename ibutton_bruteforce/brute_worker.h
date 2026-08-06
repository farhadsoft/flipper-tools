#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "brute_app.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Allocate protocols + worker + key. Returns NULL if protocol names cannot be resolved. */
BruteApp* brute_worker_setup(BruteApp* app);

/* Free worker resources. Safe to call even if setup partially failed. */
void brute_worker_teardown(BruteApp* app);

/* Start/stop the worker thread. Start must be called before any emulate. */
bool brute_worker_start(BruteApp* app);
void brute_worker_stop(BruteApp* app);

/* Stop emulating the current key, write new data into the shared key, and start emulating it.
   This is the only place that calls ibutton_worker_* / ibutton_key_* / ibutton_protocols_*. */
void brute_worker_emulate_key(
    BruteApp* app,
    iButtonProtocolId protocol_id,
    const uint8_t* data,
    size_t data_size);

/* Stop emulating (leave the worker thread running). */
void brute_worker_emulate_stop(BruteApp* app);

#ifdef __cplusplus
}
#endif
