#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/text_box.h>
#include <gui/modules/submenu.h>
#include <nfc/nfc.h>
#include <nfc/nfc_scanner.h>
#include <nfc/nfc_poller.h>
#include <nfc/nfc_listener.h>
#include <nfc/nfc_device.h>
#include <nfc/protocols/nfc_protocol.h>
#include <lfrfid/lfrfid_worker.h>
#include <toolbox/protocols/protocol_dict.h>
#include <storage/storage.h>
#include "emv.h"

#define TAG "UniCardReader"

#define ID_MAX_LEN     16
#define NFC_PHASE_MS   1200
#define LF_PHASE_MS    1600
#define ANIM_PERIOD_MS 80
#define NOTICE_MS      1600 // save result / blocked-action message dwell time

#define READ_TIMEOUT_MS 2500
#define EMV_READ_TIMEOUT_MS 8000 // fallback tries 10 AIDs before GPO/records
#define MFC_READ_TIMEOUT_MS      12000 // 2 key passes x up to 80 sector requests
#define MFUL_READ_TIMEOUT_MS     8000
#define ISO15693_READ_TIMEOUT_MS 8000  // full block dump inside activate
#define FELICA_READ_TIMEOUT_MS   6000

// Saved cards live under the app's own data folder, not the shared /ext/nfc
// and /ext/lfrfid trees, so the Load browser only ever lists this app's files.
// Files saved by earlier versions stay where they are; they are not migrated.
#define READER_SAVE_DIR EXT_PATH("apps_data/universal_card_reader")
// Same path without the /ext prefix, for the save notice: notice_l1 is 32
// bytes and the screen fits ~27 characters, so the full path would be cut
// mid-word. 31 chars + NUL fills the buffer exactly.
#define READER_SAVE_DIR_UI "apps_data/universal_card_reader"

/*
 * Worker threads and the phase timer keep running for a short while after the
 * GUI thread has torn their phase down, so their events can still be sitting
 * in the dispatcher queue. Acting on such a stale event is fatal: a second
 * nfc_poller_start() on the same Nfc instance reaches nfc_start(), which
 * furi_check()s that the instance is idle. Every event therefore carries the
 * generation it was produced in, and the GUI thread drops anything that does
 * not match the current generation.
 */
#define EVENT_ID(e)   ((e) & 0xFFu)
#define EVENT_GEN(e)  ((e) >> 8u)
#define EVENT_MAKE(id, gen) ((uint32_t)(id) | ((uint32_t)(gen) << 8u))

typedef enum {
    ReaderStateScanning,
    ReaderStateReading,
    // Unreachable today — see the TODO on ReaderEventError in reader_app.h.
    ReaderStateError,
    ReaderStateNotice, // save result / blocked-action message, auto-returns
    ReaderStateEmulating,
} ReaderState;

// Custom events posted from worker threads / the timer to the GUI thread.
typedef enum {
    ReaderEventPhaseTimeout = 100,
    ReaderEventAnimTick,
    ReaderEventNfcScanned,
    ReaderEventNfcRead,
    ReaderEventLfRead,
    // TODO(unreachable): nothing posts this — grep for EVENT_MAKE(ReaderEventError.
    // The whole path (ReaderStateError, its draw case, draw_cross(),
    // reader_handle_error() and the OK-to-rescan branch in reader_input_callback())
    // is kept for a future error source; wire it up or delete it as a whole, never half.
    ReaderEventError,
    ReaderEventActionSave,
    ReaderEventActionEmulate,
    ReaderEventActionRescan,
    ReaderEventActionLoad,
    ReaderEventActionExit,
    ReaderEventNoticeDone, // NOTICE_MS elapsed
} ReaderCustomEvent;

// Views registered with the dispatcher.
typedef enum {
    ReaderViewScan = 0,
    ReaderViewInfo = 1,
    ReaderViewActions = 2,
} ReaderView;

typedef struct {
    ReaderState state;
    uint8_t frame; // animation counter, bumped every ANIM_PERIOD_MS
    bool lf; // true while the LF phase is active
    char notice_title[24];
    char notice_l1[32];
    char notice_l2[48];
    char emu_label[32];
} ReaderModel;

// What the info/actions screens currently describe. Set by the read handlers,
// consumed by reader_do_save()/reader_do_emulate() and the payment-card check.
typedef enum {
    ReaderCardNone,
    ReaderCardNfc,
    ReaderCardLf,
    ReaderCardEmvFile, // EMV fields loaded from a .emv file; v3 files also restore the 4A transport into device
} ReaderCardKind;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    View* view;
    TextBox* text_box;
    Submenu* actions;
    FuriString* info_text; // backing store for the TextBox; must outlive the text pointer
    FuriTimer* phase_timer;
    FuriTimer* anim_timer;
    ReaderView current_view; // kept in sync by reader_switch_view()

    // NFC side
    Nfc* nfc;
    NfcScanner* scanner;
    NfcPoller* poller;
    NfcListener* listener; // NFC emulation, NULL when idle
    NfcDevice* device;
    NfcProtocol display_protocol; // most-derived protocol, used for the name/chain
    NfcProtocol poll_protocol; // protocol the poller actually runs (ids 0..11 only)
    EmvData emv; // filled by emv_read() when poll_protocol is ISO14443-4A
    uint8_t mfc_pass; // MfClassic key pass: 0 = key A, 1 = key B
    uint8_t mfc_sector; // MfClassic next sector to offer a key for

    // LF side
    ProtocolDict* dict;
    LFRFIDWorker* worker;
    bool lf_reading; // a read session is currently started
    bool lf_emulating; // an emulate session is currently active
    bool lf_thread_running; // the worker thread exists and must be joined
    ProtocolId lf_protocol;

    // Filled by the LF worker callback path, consumed on the GUI thread.
    uint8_t scratch_id[ID_MAX_LEN];
    size_t scratch_id_len;

    bool lf_phase; // which phase is currently running
    ReaderCardKind card; // what the info/actions screens currently describe
    // Read directly (no with_view_model) by reader_phase_timer_callback(),
    // which runs on the TimersSrv thread: with_view_model() from there was
    // observed to deadlock against furi_timer_start() on the GUI thread
    // (both funnel through the same FreeRTOS timer command queue/task), so
    // this mirrors the existing gen/lf_phase pattern of plain cross-thread
    // fields instead of the model's mutex.
    bool notice_active; // ReaderStateNotice is currently showing
    uint32_t gen; // bumped on every phase change; only touched by the GUI thread
} ReaderApp;

// Core services implemented in universal_card_reader.c.
void reader_stop_all(ReaderApp* app);
void reader_switch_view(ReaderApp* app, ReaderView view);
void reader_show_notice(ReaderApp* app, const char* title, const char* l1, const char* l2);
void reader_cat_hex(FuriString* out, const uint8_t* data, size_t len);
