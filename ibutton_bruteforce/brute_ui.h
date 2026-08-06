#pragma once

#include "brute_app.h"

#ifdef __cplusplus
extern "C" {
#endif

View* brute_status_view_alloc(BruteApp* app);
void brute_status_view_free(View* view);

/* Model setters — the only places that touch the view model. */
void brute_status_set_model(
    View* view,
    BruteMode mode,
    BruteState state,
    BruteProtocolItem protocol_item,
    uint32_t index,
    uint32_t total,
    const uint8_t* key_data,
    size_t key_data_len,
    uint32_t keys_per_min,
    uint32_t elapsed_ms,
    uint32_t eta_ms,
    bool notice_active,
    bool ethics_accepted);

void brute_status_set_index(View* view, uint32_t index);
void brute_status_set_state(View* view, BruteState state);
void brute_status_set_key(View* view, const uint8_t* data, size_t len);
void brute_status_set_notice(View* view, bool active);
void brute_status_set_ethics(View* view, bool accepted);
void brute_status_set_timing(View* view, uint32_t keys_per_min, uint32_t elapsed_ms, uint32_t eta_ms);

#ifdef __cplusplus
}
#endif
