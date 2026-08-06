#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/number_input.h>
#include <ibutton/ibutton_key.h>
#include <ibutton/ibutton_worker.h>
#include <ibutton/ibutton_protocols.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BRUTE_ANIM_PERIOD_MS 25U
#define BRUTE_DWELL_DEFAULT_MS 400U
#define BRUTE_GAP_DEFAULT_MS 150U
#define BRUTE_DWELL_MIN_MS 50U
#define BRUTE_GAP_MIN_MS 50U
#define BRUTE_TIMING_MIN_MS 50U
#define BRUTE_TIMING_MAX_MS 2000U
#define BRUTE_PROGRESS_SAVE_EVERY 32

#define BRUTE_APP_FOLDER "/ext/apps_data/ibutton_bruteforce"
#define BRUTE_PROGRESS_FILE BRUTE_APP_FOLDER "/progress.txt"

#define BRUTE_SEQ_FAMILY_DEFAULT 0x01U
#define BRUTE_SEQ_INDEX_DEFAULT 0U

/* View ids. All navigation goes through the dispatcher navigation callback. */
typedef enum {
    BruteViewMenu,
    BruteViewSettings,
    BruteViewStatus,
    BruteViewNumber,
    BruteViewCount,
} BruteView;

/* Menu rows. */
typedef enum {
    BruteMenuMaster,
    BruteMenuSequential,
    BruteMenuSettings,
} BruteMenuItem;

/* Settings rows. */
typedef enum {
    BruteSettingProtocol,
    BruteSettingFamily,
    BruteSettingDwell,
    BruteSettingGap,
    BruteSettingStartIndex,
    BruteSettingResume,
    BruteSettingCount,
} BruteSettingItem;

/* Running mode. */
typedef enum {
    BruteModeMaster,
    BruteModeSequential,
} BruteMode;

/* Protocol selector for sequential mode. Master mode uses each key's own protocol. */
typedef enum {
    BruteProtocolDallas,
    BruteProtocolCyfral,
    BruteProtocolMetakom,
    BruteProtocolCount,
} BruteProtocolItem;

/* Run state machine. */
typedef enum {
    BruteStateIdle,
    BruteStateNotice,
    BruteStateArmed,
    BruteStatePresent,
    BruteStateGap,
    BruteStateDone,
} BruteState;

/* Custom events posted from the timer callback or worker callbacks. Keep values < 256. */
typedef enum {
    BruteEventTick = 0,
    BruteEventMenuMaster,
    BruteEventMenuSequential,
    BruteEventMenuSettings,
    BruteEventSettingsDone,
    BruteEventNumberDone,
    BruteEventBack,
    BruteEventEmulated,
    BruteEventCount,
} BruteCustomEvent;

#define BRUTE_EVENT_MASK 0xFFU
#define EVENT_MAKE(id, gen) ((((uint32_t)(gen)) << 8) | ((uint32_t)(id) & BRUTE_EVENT_MASK))
#define EVENT_ID(ev) ((uint32_t)(ev) & BRUTE_EVENT_MASK)
#define EVENT_GEN(ev) (((uint32_t)(ev)) >> 8)

/* Status model — the only with_view_model site lives in brute_ui.c. */
typedef struct {
    BruteMode mode;
    BruteState state;
    BruteProtocolItem protocol_item;
    uint32_t index;
    uint32_t total;
    uint8_t key_data[8];
    size_t key_data_len;
    uint32_t keys_per_min;
    uint32_t elapsed_ms;
    uint32_t eta_ms;
    bool notice_active;
    bool ethics_accepted;
} BruteModel;

/* Persistent settings / progress. */
typedef struct {
    BruteMode mode;
    BruteProtocolItem protocol;
    uint32_t dwell_ms;
    uint32_t gap_ms;
    uint32_t index;
    uint32_t total;
    bool resume;
    uint8_t family;
} BruteProgress;

/* App instance. */
typedef struct {
    ViewDispatcher* view_dispatcher;
    Submenu* menu;
    VariableItemList* settings;
    NumberInput* number_input;
    View* status_view;

    iButtonProtocols* protocols;
    iButtonWorker* worker;
    iButtonKey* key;
    iButtonKey* key_dallas;
    iButtonKey* key_cyfral;
    iButtonKey* key_metakom;
    iButtonProtocolId protocol_id_dallas;
    iButtonProtocolId protocol_id_cyfral;
    iButtonProtocolId protocol_id_metakom;

    BruteMode selected_mode;
    BruteProtocolItem protocol_item;
    uint32_t dwell_ms;
    uint32_t gap_ms;
    uint32_t start_index;
    bool resume;
    uint8_t family;
    /* Which settings row opened BruteViewNumber -- Family and Start Index share
       one NumberInput view, so brute_number_input_callback() needs to know which
       app field to write the result into. */
    BruteSettingItem number_input_target;

    uint32_t current_index;
    uint32_t total_keys;
    uint32_t key_started_ms;
    uint32_t run_started_ms;
    uint32_t keys_presented;
    uint32_t gen;
    BruteState state;
    bool notice_active;
    bool ethics_accepted;
    bool worker_running;
    BruteView current_view;

    FuriTimer* tick_timer;
    BruteModel model;
} BruteApp;

/* Model setters called from anywhere except the timer callback. */
void brute_set_state(BruteApp* app, BruteState state);
void brute_set_index(BruteApp* app, uint32_t index);
void brute_set_total(BruteApp* app, uint32_t total);
void brute_set_key(BruteApp* app, const uint8_t* data, size_t len);
void brute_set_notice(BruteApp* app, bool active);
void brute_set_ethics(BruteApp* app, bool accepted);
void brute_set_protocol_item(BruteApp* app, BruteProtocolItem item);
void brute_bump_keys_presented(BruteApp* app);
void brute_update_timing(BruteApp* app, uint32_t now_ms);

/* Protocol names resolved at runtime. */
extern const char* brute_protocol_names[BruteProtocolCount];

const char* brute_protocol_item_name(BruteApp* app, BruteProtocolItem item);
const char* brute_mode_name(BruteMode mode);

#ifdef __cplusplus
}
#endif
