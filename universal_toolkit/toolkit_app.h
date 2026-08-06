#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <storage/storage.h>

#define TAG "UniToolkit"

// Custom events are packed as EVENT_MAKE(id, gen): id in the low byte,
// app->gen above it. app->gen bumps on every module exit (toolkit_exit_module,
// toolkit.c), so an event a timer/worker queued just before its module tore
// down -- FreeRTOS message queues are not flushed synchronously by
// furi_timer_stop() -- reads back with a stale generation and is dropped by
// toolkit_custom_event() instead of reaching whatever module is active next.
// Keep every per-module event id < 256: ids are module-local, since only the
// active module's `event` callback is ever dispatched, so modules never
// coordinate id ranges with each other.
#define EVENT_ID(e)         ((e) & 0xFFu)
#define EVENT_GEN(e)        ((e) >> 8u)
#define EVENT_MAKE(id, gen) ((uint32_t)(id) | ((uint32_t)(gen) << 8u))

// View id 0 is the launcher's Submenu: added once in toolkit_app_alloc(),
// never removed, always safe to switch to. Every module owns a namespace of
// 16 ids (view_base .. view_base+15) for its root view plus any sub-views
// (a settings screen, a confirm dialog, ...), so ids never collide with the
// launcher or another module -- even though only one module is ever
// registered at a time, fixed bases keep ids stable as more modules are
// appended in later phases.
#define TOOLKIT_VIEW_LAUNCHER  0u
#define TOOLKIT_VIEW_BASE_GPIO 0x10u // Phase 0 proof module
#define TOOLKIT_VIEW_BASE_CARD_READER 0x20u
#define TOOLKIT_VIEW_BASE_RFID_MULTI  0x30u
#define TOOLKIT_VIEW_BASE_SUBGHZ_REC  0x40u
#define TOOLKIT_VIEW_BASE_BLE_FINDMY  0x50u

// Append-only: values are persisted in session.log. Never renumber or reuse
// a value, even for a subsystem that is later removed.
typedef enum {
    ToolkitSubsysGpio = 0,
    ToolkitSubsysSubGhz,
    ToolkitSubsysNfc,
    ToolkitSubsysRfidLf,
    ToolkitSubsysInfrared,
    ToolkitSubsysIbutton,
    ToolkitSubsysBle,
    ToolkitSubsysBadUsb,
} ToolkitSubsys;

#define TOOLKIT_SUMMARY_MAX 48
#define TOOLKIT_PATH_MAX    96

// One entry in session.log (toolkit_log.h). Phase 0 only writes this format;
// the Phase 4 viewer is what reads it back.
typedef struct {
    uint32_t ts; // furi_hal_rtc_get_timestamp(): RTC-backed seconds
    uint8_t subsys; // ToolkitSubsys
    char summary[TOOLKIT_SUMMARY_MAX];
    char file[TOOLKIT_PATH_MAX]; // saved artefact path, "" if none
} ToolkitLogRecord;

typedef struct ToolkitApp ToolkitApp;

// A module is pure description + four callbacks; the launcher (toolkit.c)
// drives them -- a module never calls its own enter/exit/nav. Only ONE
// module is active at a time: its private state lives in app->active_ctx,
// its views occupy [view_base, view_base+16). See CLAUDE.md "Universal
// Toolkit" for the full module lifecycle contract.
typedef struct {
    const char* name; // launcher row label
    uint32_t view_base; // this module's view-id namespace base
    void (*enter)(ToolkitApp* app); // add views, alloc ctx, ACQUIRE peripheral, switch to root view
    void (*exit)(ToolkitApp* app); // RELEASE peripheral, free ctx, remove views (gen bump is the launcher's job)
    bool (*event)(ToolkitApp* app, uint32_t id); // handle this module's custom events (already gen-filtered)
    bool (*nav)(ToolkitApp* app); // Back: pop a sub-view or exit to the launcher; always returns true (handled)
} ToolkitModule;

struct ToolkitApp {
    ViewDispatcher* view_dispatcher;
    Gui* gui;
    Storage* storage;
    Submenu* launcher; // view id TOOLKIT_VIEW_LAUNCHER; core-owned, never removed

    const ToolkitModule* active; // NULL at the launcher root
    void* active_ctx; // the active module's private state; NULL when active == NULL
    // Bumped on the GUI thread: by toolkit_exit_module() on every module
    // exit, AND (Phase 1) by each wrapped module's own phase-transition
    // code (reader_start_nfc_phase(), sub_rec_capture_begin(), etc.) --
    // every module's timer callback on TimersSrv reads it to stamp
    // EVENT_MAKE(). Multi-writer by design: an app's own phase-transition
    // bump invalidates a stale in-flight event from the phase it just left
    // (same convention as when these apps ran standalone, reader_app.h /
    // recorder_app.h originally owned this counter), while the toolkit's
    // own bump on module exit invalidates anything still queued from the
    // module that just tore down. Both purposes share one counter safely
    // because every reader gen-filters against the CURRENT value at
    // dispatch time -- there is no designated sole writer to bypass.
    volatile uint32_t gen;
};

// Implemented in toolkit.c. A module's `nav` callback calls this when Back
// is pressed at its root view -- the only other caller is the launcher's own
// row callback (via toolkit_enter_module, kept file-local to toolkit.c).
void toolkit_exit_module(ToolkitApp* app);
