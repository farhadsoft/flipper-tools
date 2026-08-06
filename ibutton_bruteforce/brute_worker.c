#include "brute_worker.h"

#include <furi.h>
#include <string.h>
#include <storage/storage.h>

#include "master_keys.h"

/* The iButton worker message queue has size 1, so stop/start are synchronous
   enough for safe in-place key editing. We keep one key object per protocol
   family so that a callback from the previous protocol (still active briefly
   after stop) never reads data formatted for a different protocol. */

static void brute_worker_emulate_callback(void* context, bool emulated) {
    BruteApp* app = context;
    if(emulated && app) {
        view_dispatcher_send_custom_event(app->view_dispatcher, EVENT_MAKE(BruteEventEmulated, app->gen));
    }
}

static iButtonKey* brute_worker_key_for_protocol(BruteApp* app, iButtonProtocolId protocol_id) {
    if(protocol_id == app->protocol_id_dallas) {
        return app->key_dallas;
    } else if(protocol_id == app->protocol_id_cyfral) {
        return app->key_cyfral;
    } else if(protocol_id == app->protocol_id_metakom) {
        return app->key_metakom;
    }
    /* Sequential mode uses DSGeneric, which is also a Dallas-family protocol. */
    return app->key_dallas;
}

BruteApp* brute_worker_setup(BruteApp* app) {
    furi_check(app);

    app->protocols = ibutton_protocols_alloc();
    if(!app->protocols) {
        FURI_LOG_E("Brute", "ibutton_protocols_alloc failed");
        return NULL;
    }

    Storage* storage = furi_record_open(RECORD_STORAGE);
    const bool master_keys_ok = brute_master_keys_load(storage, app->protocols);
    furi_record_close(RECORD_STORAGE);
    if(!master_keys_ok) {
        return NULL;
    }

    app->protocol_id_dallas = ibutton_protocols_get_id_by_name(app->protocols, BRUTE_PROTOCOL_DALLAS);
    app->protocol_id_cyfral = ibutton_protocols_get_id_by_name(app->protocols, BRUTE_PROTOCOL_CYFRAL);
    app->protocol_id_metakom = ibutton_protocols_get_id_by_name(app->protocols, BRUTE_PROTOCOL_METAKOM);

    if(app->protocol_id_dallas == iButtonProtocolIdInvalid ||
       app->protocol_id_cyfral == iButtonProtocolIdInvalid ||
       app->protocol_id_metakom == iButtonProtocolIdInvalid) {
        FURI_LOG_E("Brute", "failed to resolve one or more protocol names");
        return NULL;
    }

    const size_t max_data_size = ibutton_protocols_get_max_data_size(app->protocols);

    app->key_dallas = ibutton_key_alloc(max_data_size);
    app->key_cyfral = ibutton_key_alloc(max_data_size);
    app->key_metakom = ibutton_key_alloc(max_data_size);
    app->key = app->key_dallas; /* Legacy single-key pointer not used for stepping. */

    if(!app->key_dallas || !app->key_cyfral || !app->key_metakom) {
        FURI_LOG_E("Brute", "ibutton_key_alloc failed");
        return NULL;
    }

    app->worker = ibutton_worker_alloc(app->protocols);
    if(!app->worker) {
        FURI_LOG_E("Brute", "ibutton_worker_alloc failed");
        return NULL;
    }

    ibutton_worker_emulate_set_callback(app->worker, brute_worker_emulate_callback, app);
    app->worker_running = false;
    return app;
}

void brute_worker_teardown(BruteApp* app) {
    if(!app) return;

    if(app->worker) {
        if(app->worker_running) {
            ibutton_worker_stop_thread(app->worker);
            app->worker_running = false;
        }
        ibutton_worker_free(app->worker);
        app->worker = NULL;
    }
    if(app->key_dallas) {
        ibutton_key_free(app->key_dallas);
        app->key_dallas = NULL;
    }
    if(app->key_cyfral) {
        ibutton_key_free(app->key_cyfral);
        app->key_cyfral = NULL;
    }
    if(app->key_metakom) {
        ibutton_key_free(app->key_metakom);
        app->key_metakom = NULL;
    }
    app->key = NULL;
    if(app->protocols) {
        ibutton_protocols_free(app->protocols);
        app->protocols = NULL;
    }

    brute_master_keys_free();
}

bool brute_worker_start(BruteApp* app) {
    furi_check(app);
    furi_check(app->worker);
    if(app->worker_running) return true;

    ibutton_worker_start_thread(app->worker);
    app->worker_running = true;
    return true;
}

void brute_worker_stop(BruteApp* app) {
    furi_check(app);
    if(!app->worker || !app->worker_running) return;

    ibutton_worker_stop_thread(app->worker);
    app->worker_running = false;
}

void brute_worker_emulate_stop(BruteApp* app) {
    furi_check(app);
    if(!app->worker || !app->worker_running) return;

    ibutton_worker_stop(app->worker);
}

void brute_worker_emulate_key(
    BruteApp* app,
    iButtonProtocolId protocol_id,
    const uint8_t* data,
    size_t data_size) {
    furi_check(app);
    furi_check(app->worker);
    furi_check(app->worker_running);

    iButtonKey* key = brute_worker_key_for_protocol(app, protocol_id);
    furi_check(key);

    /* Stop current emulation before modifying the shared key. */
    ibutton_worker_stop(app->worker);

    ibutton_key_reset(key);
    ibutton_key_set_protocol_id(key, protocol_id);

    iButtonEditableData editable;
    ibutton_protocols_get_editable_data(app->protocols, key, &editable);

    const size_t copy_size = data_size < editable.size ? data_size : editable.size;
    memset(editable.ptr, 0, editable.size);
    if(copy_size > 0) {
        memcpy(editable.ptr, data, copy_size);
    }

    ibutton_protocols_apply_edits(app->protocols, key);

    /* Start emulating the new key. */
    ibutton_worker_emulate_start(app->worker, key);
}
