#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/text_box.h>
#include <gui/modules/submenu.h>
#include "rfid_backend.h"

#define TAG "RfidMultiReader"

#define HF_PHASE_MS    1200
#define LF_PHASE_MS    1600
#define ANIM_PERIOD_MS 80
#define NOTICE_MS      1800
// Bounds TextBox's O(n) re-layout on huge dumps (Classic 4K is ~15 KB uncapped).
// Shared with rfid_multi_reader.c, which reserves info_text to this size.
#define CARD_INFO_MAX 8192

// Generation stamping: worker threads and the timer keep running briefly after
// the GUI thread tore their phase down, so their events can still be queued.
// Acting on a stale one is fatal (a second nfc_poller_start() on the same Nfc
// instance reaches nfc_start(), which furi_check()s that the instance is idle).
// Every RfidCustomEvent value MUST stay below 256; the upper bits carry the gen.
#define EVENT_ID(e)         ((e) & 0xFFu)
#define EVENT_GEN(e)        ((e) >> 8u)
#define EVENT_MAKE(id, gen) ((uint32_t)(id) | ((uint32_t)(gen) << 8u))

typedef enum {
    RfidEventAnimTick = 100, // gen-stamped like every other event; the toolkit filters it centrally now
    RfidEventPhaseTimeout,
    RfidEventReadTimeout,
    RfidEventNoticeDone,
    RfidEventDetected,
    RfidEventRead,
    RfidEventMenuAuto, // menu item indices double as event ids
    RfidEventMenuHf,
    RfidEventMenuLf,
    RfidEventMenuUhf,
} RfidCustomEvent;

typedef enum {
    RfidViewMenu = 0,
    RfidViewStatus = 1, // custom animated View
    RfidViewInfo = 2, // TextBox
} RfidView;

typedef enum {
    RfidModeAuto,
    RfidModeHf,
    RfidModeLf,
} RfidMode;

typedef enum {
    RfidStateIdle, // menu is up, no radio
    RfidStateScanning,
    RfidStateReading,
    RfidStateResult, // report on screen
    RfidStateNotice,
} RfidState;

// Which meaning the one-shot timer currently carries. Read by the timer
// callback on the TimersSrv thread, so it is a plain field: with_view_model()
// from a timer callback deadlocks against furi_timer_start() on the GUI thread
// (both funnel through the same FreeRTOS timer command queue) — a confirmed
// freeze in universal_card_reader.
typedef enum {
    RfidTimerNone,
    RfidTimerPhase,
    RfidTimerRead,
    RfidTimerNotice,
} RfidTimerRole;

typedef struct {
    RfidState state;
    uint8_t frame;
    uint8_t battery; // furi_hal_power_get_pct(), refreshed on anim bump
    char band[24]; // "13.56 MHz HF"
    char mode[16]; // "Auto" / "HF only" / "LF only"
    char notice_title[24];
    char notice_l1[32];
    char notice_l2[40];
} RfidModel;

typedef struct ToolkitApp ToolkitApp; // forward declaration; full type in universal_toolkit/toolkit_app.h

typedef struct {
    ViewDispatcher* view_dispatcher;
    View* status;
    TextBox* text_box;
    Submenu* menu;
    FuriString* info_text; // TextBox backing store; must outlive the text pointer
    FuriTimer* timer; // one-shot: phase / read / notice
    FuriTimer* anim_timer; // periodic
    RfidView current_view;

    RfidBackend* backends[RfidBandCount]; // indexed by RfidBand
    RfidBackend* rotation[RfidBandCount]; // available backends for the current mode
    size_t rotation_len;
    size_t rot_idx;
    RfidBackend* active; // backend currently scanning/read/described; NULL on the menu

    RfidMode mode;
    RfidState state; // GUI-thread source of truth; the model gets a copy
    volatile RfidTimerRole timer_role; // cross-thread, see above; volatile because TimersSrv reads it

    ToolkitApp* toolkit; // set by the module wrapper; NULL when standalone
    bool module_mode; // true when running as a toolkit module
    uint32_t view_base; // this instance's view-id namespace base
} RfidApp;

// Core services implemented in rfid_multi_reader.c.
void rfid_stop_all(RfidApp* app);
void rfid_switch_view(RfidApp* app, RfidView view);
void rfid_show_notice(RfidApp* app, const char* title, const char* l1, const char* l2);
void rfid_start_scan(RfidApp* app);
void rfid_advance_phase(RfidApp* app);
RfidApp* rfid_app_alloc(ViewDispatcher* view_dispatcher);
void rfid_app_free(RfidApp* app);
bool rfid_custom_event_callback(void* context, uint32_t event);
bool rfid_navigation_callback(void* context);
