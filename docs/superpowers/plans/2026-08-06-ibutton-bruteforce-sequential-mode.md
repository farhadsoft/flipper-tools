# iButton Brute Force: Sequential/Master Mode Effectiveness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move the iButton Brute Force master-key table from a compiled-in C
array to a user-editable SD file, fix Cyfral sequential mode to cover its
full 65536-key space exactly once (currently wraps and duplicates past
65536), and expose the Dallas family byte as a real Settings control instead
of a hand-edited persistence field.

**Architecture:** `master_keys.c` gains a load/parse/free lifecycle
(FlipperFormat, repeated-record text file, single-pass growth-array parse)
that replaces its static initializer; `brute_worker_setup()`/`_teardown()`
call it instead of the old alloc-time self-check. `ibutton_bruteforce.c`
gets a protocol-aware `total_keys` in `brute_run_start()` and a new `Family`
settings row sharing the existing NumberInput flow with Start Index.

**Tech Stack:** Flipper Zero FAP (C, `ufbt`), FlipperFormat, iButton worker
API. Target: hardware 7 / f7 SDK, Momentum `mntm-dev` firmware — see this
repo's `CLAUDE.md` Step 0 before flashing if the device/SDK pairing is ever
in doubt.

**No unit test framework exists in this codebase; verification is
build-then-device, by convention (see `CLAUDE.md`).** Every code task ends
with a `ufbt` build-verify step in place of a test run. Full on-device
behavior is verified once, in Task 6, after all code tasks land — this
mirrors how this repo already verifies FAPs and avoids serializing a device
in the loop after every single micro-step.

Spec: `docs/superpowers/specs/2026-08-06-ibutton-bruteforce-sequential-mode-design.md`

---

### Task 1: Master-key file format — parser and seed

**Files:**
- Modify: `ibutton_bruteforce/master_keys.h` (full rewrite)
- Modify: `ibutton_bruteforce/master_keys.c` (full rewrite)

- [ ] **Step 1: Rewrite `master_keys.h`**

```c
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <storage/storage.h>
#include <ibutton/ibutton_protocols.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Protocol names are resolved at runtime, so enum drift between forks cannot bite us. */
#define BRUTE_PROTOCOL_DALLAS "DS1990"
#define BRUTE_PROTOCOL_CYFRAL "Cyfral"
#define BRUTE_PROTOCOL_METAKOM "Metakom"
#define BRUTE_PROTOCOL_DALLAS_GENERIC "DSGeneric"

typedef struct {
    char name[40]; /* Human label, truncated if longer. */
    iButtonProtocolId protocol_id; /* Resolved at load time. */
    uint8_t data[8]; /* Zero-padded past the protocol's real length. */
} MasterKey;

/* Curated table of documented master keys, loaded from
   /ext/apps_data/ibutton_bruteforce/master_keys.txt. If the file is missing, a small
   seed of clearly-labeled example entries is written first so the app runs out of the
   box; replace them with keys from documented, authorized sources before any real use.
   Heap-allocated by brute_master_keys_load(); release with brute_master_keys_free(). */
extern MasterKey* master_keys;
extern size_t master_keys_count;

/* Seeds the file if missing, then loads, parses, and validates every entry --
   protocol whitelist, protocol name resolution, Dallas CRC8. Fails closed: on any
   error master_keys/master_keys_count are left NULL/0 and false is returned. */
bool brute_master_keys_load(Storage* storage, iButtonProtocols* protocols);

/* Releases the table. Safe to call even if load was never called or failed. */
void brute_master_keys_free(void);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Rewrite `master_keys.c`**

```c
#include "master_keys.h"

#include <furi.h>
#include <string.h>
#include <storage/storage.h>
#include <lib/flipper_format/flipper_format.h>

#include "brute_app.h" /* BRUTE_APP_FOLDER */
#include "crc8_dallas.h"

#define BRUTE_MASTER_KEYS_FILE BRUTE_APP_FOLDER "/master_keys.txt"
#define BRUTE_MASTER_KEYS_INITIAL_CAP 8U

MasterKey* master_keys = NULL;
size_t master_keys_count = 0;

/* Seed written to master_keys.txt the first time the app runs on a card that doesn't
   have one yet -- the same six example entries the static table used to ship with.
   Deliberately trivial, clearly-labeled placeholders: replace with keys from
   documented, authorized sources before any real use. */
typedef struct {
    const char* name;
    const char* protocol_name;
    uint8_t data[8];
    uint8_t data_len;
} MasterKeySeed;

static const MasterKeySeed master_keys_seed[] = {
    {"Example DS1990 all-zero serial",
     BRUTE_PROTOCOL_DALLAS,
     {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3D},
     8},
    {"Example DS1990 all-ones serial",
     BRUTE_PROTOCOL_DALLAS,
     {0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x2F},
     8},
    {"Example Cyfral 0x0000", BRUTE_PROTOCOL_CYFRAL, {0x00, 0x00, 0, 0, 0, 0, 0, 0}, 2},
    {"Example Cyfral 0xFFFF", BRUTE_PROTOCOL_CYFRAL, {0xFF, 0xFF, 0, 0, 0, 0, 0, 0}, 2},
    {"Example Metakom 0x00000000",
     BRUTE_PROTOCOL_METAKOM,
     {0x00, 0x00, 0x00, 0x00, 0, 0, 0, 0},
     4},
    {"Example Metakom 0xFFFFFFFF",
     BRUTE_PROTOCOL_METAKOM,
     {0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0},
     4},
};

/* Returns the on-wire byte count for a protocol name, or 0 if not recognized --
   0 doubles as the "not in whitelist" signal for brute_master_keys_parse(). */
static uint8_t brute_protocol_data_len(const char* protocol_name) {
    if(strcmp(protocol_name, BRUTE_PROTOCOL_DALLAS) == 0) return 8;
    if(strcmp(protocol_name, BRUTE_PROTOCOL_CYFRAL) == 0) return 2;
    if(strcmp(protocol_name, BRUTE_PROTOCOL_METAKOM) == 0) return 4;
    return 0;
}

static void brute_master_keys_write_seed(Storage* storage) {
    storage_common_mkdir(storage, BRUTE_APP_FOLDER);

    FlipperFormat* ff = flipper_format_buffered_file_alloc(storage);
    do {
        if(!flipper_format_buffered_file_open_always(ff, BRUTE_MASTER_KEYS_FILE)) break;
        if(!flipper_format_write_header_cstr(ff, "IBF Master Keys", 1)) break;

        bool ok = true;
        for(size_t i = 0; i < COUNT_OF(master_keys_seed) && ok; ++i) {
            const MasterKeySeed* seed = &master_keys_seed[i];
            ok = flipper_format_write_string_cstr(ff, "Name", seed->name) &&
                 flipper_format_write_string_cstr(ff, "Protocol", seed->protocol_name) &&
                 flipper_format_write_hex(ff, "Data", seed->data, seed->data_len);
        }
        if(!ok) {
            FURI_LOG_E("Brute", "master_keys.txt: failed writing seed");
        }
    } while(false);
    flipper_format_free(ff);
}

/* Single-pass parse into a doubling-growth array -- avoids both a two-pass
   count-then-rewind (rewind-to-where is ambiguous without firmware source) and
   opening the file twice. Fails closed: stops at the first bad record. */
static bool brute_master_keys_parse(FlipperFormat* ff, iButtonProtocols* protocols) {
    FuriString* name = furi_string_alloc();
    FuriString* protocol = furi_string_alloc();

    size_t cap = BRUTE_MASTER_KEYS_INITIAL_CAP;
    MasterKey* keys = malloc(cap * sizeof(MasterKey));
    size_t count = 0;
    bool ok = true;

    while(flipper_format_read_string(ff, "Name", name)) {
        if(count == cap) {
            cap *= 2;
            keys = realloc(keys, cap * sizeof(MasterKey));
        }
        MasterKey* key = &keys[count];
        snprintf(key->name, sizeof(key->name), "%s", furi_string_get_cstr(name));

        if(!flipper_format_read_string(ff, "Protocol", protocol)) {
            FURI_LOG_E("Brute", "master key %zu: missing Protocol", count);
            ok = false;
            break;
        }
        const char* protocol_cstr = furi_string_get_cstr(protocol);
        const uint8_t data_len = brute_protocol_data_len(protocol_cstr);
        if(data_len == 0) {
            FURI_LOG_E(
                "Brute", "master key %zu: protocol not in whitelist: %s", count, protocol_cstr);
            ok = false;
            break;
        }

        key->protocol_id = ibutton_protocols_get_id_by_name(protocols, protocol_cstr);
        if(key->protocol_id == iButtonProtocolIdInvalid) {
            FURI_LOG_E("Brute", "master key %zu: unknown protocol %s", count, protocol_cstr);
            ok = false;
            break;
        }

        memset(key->data, 0, sizeof(key->data));
        if(!flipper_format_read_hex(ff, "Data", key->data, data_len)) {
            FURI_LOG_E("Brute", "master key %zu: missing/short Data", count);
            ok = false;
            break;
        }

        if(strcmp(protocol_cstr, BRUTE_PROTOCOL_DALLAS) == 0) {
            const uint8_t crc = crc8_dallas(key->data, 7);
            if(crc != key->data[7]) {
                FURI_LOG_E(
                    "Brute",
                    "master key %zu: CRC mismatch: got %02X, want %02X",
                    count,
                    key->data[7],
                    crc);
                ok = false;
                break;
            }
        }

        count++;
    }

    furi_string_free(name);
    furi_string_free(protocol);

    if(!ok) {
        free(keys);
        return false;
    }
    if(count == 0) {
        FURI_LOG_E("Brute", "master_keys.txt: no entries");
        free(keys);
        return false;
    }

    master_keys = keys;
    master_keys_count = count;
    return true;
}

bool brute_master_keys_load(Storage* storage, iButtonProtocols* protocols) {
    furi_check(storage);
    furi_check(protocols);

    if(!storage_file_exists(storage, BRUTE_MASTER_KEYS_FILE)) {
        FURI_LOG_I("Brute", "master_keys.txt missing, writing seed");
        brute_master_keys_write_seed(storage);
    }

    FlipperFormat* ff = flipper_format_buffered_file_alloc(storage);
    bool ok = false;

    do {
        if(!flipper_format_buffered_file_open_existing(ff, BRUTE_MASTER_KEYS_FILE)) {
            FURI_LOG_E("Brute", "master_keys.txt: cannot open");
            break;
        }

        FuriString* filetype = furi_string_alloc();
        uint32_t version = 0;
        const bool header_ok = flipper_format_read_header(ff, filetype, &version);
        const bool header_match =
            header_ok && furi_string_equal_str(filetype, "IBF Master Keys") && version == 1;
        furi_string_free(filetype);
        if(!header_match) {
            FURI_LOG_E("Brute", "master_keys.txt: bad header");
            break;
        }

        ok = brute_master_keys_parse(ff, protocols);
    } while(false);

    flipper_format_free(ff);

    if(ok) {
        FURI_LOG_I("Brute", "master_keys self-check passed, %zu entries", master_keys_count);
    } else {
        FURI_LOG_E("Brute", "master_keys self-check failed");
    }
    return ok;
}

void brute_master_keys_free(void) {
    free(master_keys);
    master_keys = NULL;
    master_keys_count = 0;
}
```

- [ ] **Step 3: Build-verify**

Run: `cd ibutton_bruteforce && ufbt`
Expected: fails to link — `brute_worker.c` still calls the removed
`brute_master_keys_self_check()`. That's Task 2. Confirm the *compile* errors
are only in `brute_worker.c` (undefined reference / implicit declaration for
`brute_master_keys_self_check`), not in `master_keys.c` itself — that isolates
this task's code as correct before moving on.

- [ ] **Step 4: Commit**

```bash
git add ibutton_bruteforce/master_keys.h ibutton_bruteforce/master_keys.c
git commit -m "feat(ibutton_bruteforce): load master keys from SD file instead of compiled table"
```

---

### Task 2: Wire the loader into worker setup/teardown

**Files:**
- Modify: `ibutton_bruteforce/brute_worker.c:1-11` (includes), `:32-54` (setup), `:79-107` (teardown)

- [ ] **Step 1: Add the storage include**

Current top of file (lines 1-6):
```c
#include "brute_worker.h"

#include <furi.h>
#include <string.h>

#include "master_keys.h"
```
Replace with:
```c
#include "brute_worker.h"

#include <furi.h>
#include <string.h>
#include <storage/storage.h>

#include "master_keys.h"
```

- [ ] **Step 2: Replace the self-check call in `brute_worker_setup()`**

Find:
```c
    app->protocols = ibutton_protocols_alloc();
    if(!app->protocols) {
        FURI_LOG_E("Brute", "ibutton_protocols_alloc failed");
        return NULL;
    }

    if(!brute_master_keys_self_check(app->protocols)) {
        return NULL;
    }
```
Replace with:
```c
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
```

- [ ] **Step 3: Free the table in `brute_worker_teardown()`**

Find the end of the function:
```c
    app->key = NULL;
    if(app->protocols) {
        ibutton_protocols_free(app->protocols);
        app->protocols = NULL;
    }
}
```
Replace with:
```c
    app->key = NULL;
    if(app->protocols) {
        ibutton_protocols_free(app->protocols);
        app->protocols = NULL;
    }

    brute_master_keys_free();
}
```

(`brute_master_keys_free()` is `free(NULL)`-safe, so this is correct even
when `brute_worker_setup()` returned NULL before the table ever loaded —
matches this function's existing "safe to call even if setup partially
failed" contract, documented in `brute_worker.h`.)

- [ ] **Step 4: Build-verify**

Run: `cd ibutton_bruteforce && ufbt`
Expected: clean build, `dist/ibutton_bruteforce.fap` produced. This confirms
Task 1 + Task 2 together compile and link.

- [ ] **Step 5: Commit**

```bash
git add ibutton_bruteforce/brute_worker.c
git commit -m "feat(ibutton_bruteforce): load/free master keys from worker setup/teardown"
```

---

### Task 3: Cyfral exhaustive keyspace fix

**Files:**
- Modify: `ibutton_bruteforce/ibutton_bruteforce.c:12` (add define), `:323-329` (total_keys)

- [ ] **Step 1: Add the keyspace constant**

Find:
```c
#define BRUTE_NOTICE_MS 2500U
#define BRUTE_SEQ_TOTAL 100000U
```
Replace with:
```c
#define BRUTE_NOTICE_MS 2500U
#define BRUTE_SEQ_TOTAL 100000U
/* Cyfral keys are 2 bytes -- the entire keyspace is small enough to always
   walk exhaustively rather than apply the same bounded bruteforce budget
   Dallas/Metakom use for their much larger (2^32) spaces. */
#define BRUTE_CYFRAL_KEYSPACE 65536U
```

- [ ] **Step 2: Make `total_keys` protocol-aware in `brute_run_start()`**

Find:
```c
    } else {
        app->total_keys = BRUTE_SEQ_TOTAL;
        app->current_index = app->start_index;
        if(resume_index > app->start_index && resume_index < app->total_keys) {
            app->current_index = resume_index;
        }
    }
```
Replace with:
```c
    } else {
        app->total_keys = (app->protocol_item == BruteProtocolCyfral) ? BRUTE_CYFRAL_KEYSPACE :
                                                                          BRUTE_SEQ_TOTAL;
        app->current_index = app->start_index;
        if(resume_index > app->start_index && resume_index < app->total_keys) {
            app->current_index = resume_index;
        }
    }
```

- [ ] **Step 3: Build-verify**

Run: `cd ibutton_bruteforce && ufbt`
Expected: clean build.

- [ ] **Step 4: Commit**

```bash
git add ibutton_bruteforce/ibutton_bruteforce.c
git commit -m "fix(ibutton_bruteforce): cap Cyfral sequential walk at its real 65536-key space"
```

---

### Task 4: Family setting in Settings UI

**Files:**
- Modify: `ibutton_bruteforce/brute_app.h:48-56` (enum), `:146-153` (struct field)
- Modify: `ibutton_bruteforce/ibutton_bruteforce.c:171-192` (callbacks), `:194-223` (settings_setup)

- [ ] **Step 1: Add `BruteSettingFamily` to the enum**

Find (`brute_app.h`):
```c
/* Settings rows. */
typedef enum {
    BruteSettingProtocol,
    BruteSettingDwell,
    BruteSettingGap,
    BruteSettingStartIndex,
    BruteSettingResume,
    BruteSettingCount,
} BruteSettingItem;
```
Replace with:
```c
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
```

- [ ] **Step 2: Add the routing field to `BruteApp`**

Find (`brute_app.h`):
```c
    BruteMode selected_mode;
    BruteProtocolItem protocol_item;
    uint32_t dwell_ms;
    uint32_t gap_ms;
    uint32_t start_index;
    bool resume;
    uint8_t family;
```
Replace with:
```c
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
```

- [ ] **Step 3: Build-verify header changes compile**

Run: `cd ibutton_bruteforce && ufbt`
Expected: **clean build.** A new enum value and a new struct field are both
backward-compatible in C — nothing breaks yet. What's now true but not yet
*used* is that inserting `BruteSettingFamily` shifts `BruteSettingDwell`,
`BruteSettingGap`, `BruteSettingStartIndex`, and `BruteSettingResume` up by
one each (plain enums auto-increment) — that's a latent logic mismatch
against `brute_settings_setup()`'s still-old row order, not a compiler
error. Steps 4-5 below make the row order match the new enum order again;
until then, don't run the app on-device from this intermediate state.

- [ ] **Step 4: Route both settings through `number_input_target`**

Find (`ibutton_bruteforce.c`):
```c
static void brute_settings_enter_callback(void* context, uint32_t index) {
    BruteApp* app = context;
    if(index == BruteSettingStartIndex) {
        /* Open number input for start index. */
        number_input_set_header_text(app->number_input, "Start index");
        number_input_set_result_callback(
            app->number_input,
            brute_number_input_callback,
            app,
            (int32_t)app->start_index,
            0,
            999999);
        brute_switch_view(app, BruteViewNumber);
    }
}

/* Number input callback. */
static void brute_number_input_callback(void* context, int32_t number) {
    BruteApp* app = context;
    app->start_index = (uint32_t)number;
    view_dispatcher_send_custom_event(app->view_dispatcher, EVENT_MAKE(BruteEventNumberDone, app->gen));
}
```
Replace with:
```c
static void brute_settings_enter_callback(void* context, uint32_t index) {
    BruteApp* app = context;
    if(index == BruteSettingStartIndex) {
        app->number_input_target = BruteSettingStartIndex;
        number_input_set_header_text(app->number_input, "Start index");
        number_input_set_result_callback(
            app->number_input,
            brute_number_input_callback,
            app,
            (int32_t)app->start_index,
            0,
            999999);
        brute_switch_view(app, BruteViewNumber);
    } else if(index == BruteSettingFamily) {
        app->number_input_target = BruteSettingFamily;
        number_input_set_header_text(app->number_input, "Family (0-255)");
        number_input_set_result_callback(
            app->number_input, brute_number_input_callback, app, (int32_t)app->family, 0, 255);
        brute_switch_view(app, BruteViewNumber);
    }
}

/* Number input callback -- shared by Start Index and Family; number_input_target
   (set in brute_settings_enter_callback() just above) says which field to write. */
static void brute_number_input_callback(void* context, int32_t number) {
    BruteApp* app = context;
    if(app->number_input_target == BruteSettingFamily) {
        app->family = (uint8_t)number;
    } else {
        app->start_index = (uint32_t)number;
    }
    view_dispatcher_send_custom_event(app->view_dispatcher, EVENT_MAKE(BruteEventNumberDone, app->gen));
}
```

- [ ] **Step 5: Add the Family row to `brute_settings_setup()`**

Find:
```c
static void brute_settings_setup(BruteApp* app) {
    app->settings = variable_item_list_alloc();
    variable_item_list_set_enter_callback(app->settings, brute_settings_enter_callback, app);

    VariableItem* item;
    char buf[16];

    item = variable_item_list_add(app->settings, "Protocol", BruteProtocolCount, brute_settings_protocol_change, app);
    variable_item_set_current_value_index(item, app->protocol_item);
    variable_item_set_current_value_text(item, brute_protocol_item_name(app, app->protocol_item));

    item = variable_item_list_add(app->settings, "Dwell", brute_timing_index_count(), brute_settings_dwell_change, app);
```
Replace with:
```c
static void brute_settings_setup(BruteApp* app) {
    app->settings = variable_item_list_alloc();
    variable_item_list_set_enter_callback(app->settings, brute_settings_enter_callback, app);

    VariableItem* item;
    char buf[16];

    item = variable_item_list_add(app->settings, "Protocol", BruteProtocolCount, brute_settings_protocol_change, app);
    variable_item_set_current_value_index(item, app->protocol_item);
    variable_item_set_current_value_text(item, brute_protocol_item_name(app, app->protocol_item));

    /* Sequential-mode only (Dallas uses it; Cyfral/Metakom generators ignore it) --
       shown unconditionally rather than rebuilding the list on protocol change. */
    item = variable_item_list_add(app->settings, "Family", 1, NULL, app);
    variable_item_set_current_value_index(item, 0);
    snprintf(buf, sizeof(buf), "0x%02X", (unsigned)app->family);
    variable_item_set_current_value_text(item, buf);

    item = variable_item_list_add(app->settings, "Dwell", brute_timing_index_count(), brute_settings_dwell_change, app);
```

(The rest of `brute_settings_setup()` — Gap, Start index, Resume rows — is
unchanged; only the insertion point above matters.)

- [ ] **Step 6: Build-verify**

Run: `cd ibutton_bruteforce && ufbt`
Expected: clean build, `dist/ibutton_bruteforce.fap` produced.

- [ ] **Step 7: Commit**

```bash
git add ibutton_bruteforce/brute_app.h ibutton_bruteforce/ibutton_bruteforce.c
git commit -m "feat(ibutton_bruteforce): expose Dallas family byte as a Settings row"
```

---

### Task 5: Documentation

**Files:**
- Modify: `ibutton_bruteforce/README.md`

- [ ] **Step 1: Update the "What it does" sequential-walk bullet**

Find:
```
- **Sequential walk** — generates keys from a chosen Dallas family code plus an
  incrementing 32-bit serial, with a locally computed Dallas CRC8. The walk is
  bounded to 100 000 keys by default; at the same 550 ms period that is ~15
  hours. The full 48-bit serial space is ~2.8 × 10^14 keys and would take
  billions of years at this pace, so the sequential mode is only useful when
  the search space is already reduced (partial-known serial, weak legacy
  ranges).
```
Replace with:
```
- **Sequential walk** — generates keys from an incrementing serial, with a
  locally computed Dallas CRC8 for the Dallas protocol. Coverage depends on
  protocol: **Cyfral is exhaustive** — its keyspace is exactly 65 536 keys
  (2 bytes), small enough to walk in full every run. **Dallas and Metakom are
  bounded, not exhaustive** — 100 000 keys by default (~15 hours at the
  550 ms default period); Dallas's 32-bit serial space is ~4.3 × 10^9 keys
  and Metakom's is comparable, both impractical to exhaust, so bounded mode
  is only useful when the search space is already reduced (partial-known
  serial, weak legacy ranges). The Dallas family byte is set via the
  **Family** setting (default 0x01 / DS1990A).
```

- [ ] **Step 2: Add the Family bullet to the Settings list**

Find:
```
4. **Settings** lets you change:
   - **Protocol** — for sequential mode only (DSGeneric/Dallas, Cyfral, Metakom)
   - **Dwell** — how long each key is emulated (50–2000 ms, default 400 ms)
```
Replace with:
```
4. **Settings** lets you change:
   - **Protocol** — for sequential mode only (DSGeneric/Dallas, Cyfral, Metakom)
   - **Family** — Dallas family byte for sequential mode (0-255, default
     0x01 / DS1990A). Shown for every protocol; Cyfral/Metakom ignore it.
   - **Dwell** — how long each key is emulated (50–2000 ms, default 400 ms)
```

- [ ] **Step 3: Document the master-key file in Storage**

Find:
```
## Storage

Progress and settings are persisted at
**`/ext/apps_data/ibutton_bruteforce/progress.txt`** in FlipperFormat.
The file stores mode, protocol, current index, total, dwell, gap, family
code, start index, resume flag, and the one-time ethics-accepted flag.
It is written when the run stops and every 32 keys to bound SD wear.

The app never creates `.ibtn` files, never writes to the shared `/ext/ibutton/`
tree, and never reads keys from the file system.
```
Replace with:
```
## Storage

Progress and settings are persisted at
**`/ext/apps_data/ibutton_bruteforce/progress.txt`** in FlipperFormat.
The file stores mode, protocol, current index, total, dwell, gap, family
code, start index, resume flag, and the one-time ethics-accepted flag.
It is written when the run stops and every 32 keys to bound SD wear.

The master-key table lives at
**`/ext/apps_data/ibutton_bruteforce/master_keys.txt`**, also FlipperFormat,
as a sequence of `Name` / `Protocol` / `Data` records (one `Data` byte count
per protocol: DS1990=8, Cyfral=2, Metakom=4). Edit it directly on the SD card
to add your own keys. If it's missing, the app writes a small seed of
clearly-labeled example entries and loads that. If it exists but fails to
parse or fails validation, the app refuses to start rather than present keys
it cannot trust — see Troubleshooting.

The app never creates `.ibtn` files, never writes to the shared `/ext/ibutton/`
tree, and never reads keys from the standard `/ext/ibutton/` file browser.
```

- [ ] **Step 4: Fix the stale Cyfral/Metakom sequential-mode limitation**

Find:
```
- **Cyfral and Metakom master keys are master-list only** in v1. Their code
  spaces and reader behaviours differ from Dallas, and sequential mode for them
  needs hardware evidence before it is enabled.
```
Replace with:
```
- **Metakom sequential mode is bounded, like Dallas** — its keyspace (2^32) is
  too large to exhaust, so the same 100 000-key default budget applies.
  Cyfral sequential mode is exhaustive (see above) — its whole keyspace fits
  inside a single run.
```

- [ ] **Step 5: Update the file-structure table row**

Find:
```
| `master_keys.c/h` | Curated master-key table + alloc-time self-check |
```
Replace with:
```
| `master_keys.c/h` | Curated master-key table, loaded/validated from `master_keys.txt` on the SD card |
```

- [ ] **Step 6: Update the Troubleshooting section**

Find:
```
## Troubleshooting

If the app exits immediately on launch, check `log error` for a
`master_keys self-check` line. This means either:
- the table is empty,
- a table entry names a protocol the firmware does not recognize, or
- a Dallas entry has a CRC mismatch.

The app refuses to start rather than present keys it cannot trust.
```
Replace with:
```
## Troubleshooting

If the app exits immediately on launch, check `log error` for a
`master_keys self-check` line. This means either:
- `master_keys.txt` has no entries,
- an entry names a protocol the firmware does not recognize, or
- a Dallas entry has a CRC mismatch.

The app refuses to start rather than present keys it cannot trust. Delete
`/ext/apps_data/ibutton_bruteforce/master_keys.txt` to get a fresh seed file
on the next launch.
```

- [ ] **Step 7: Commit**

```bash
git add ibutton_bruteforce/README.md
git commit -m "docs(ibutton_bruteforce): document master_keys.txt, Family setting, Cyfral exhaustive mode"
```

---

### Task 6: On-device verification

**Files:** none (verification only)

Needs a connected device (see this repo's `CLAUDE.md` Step 0 for
firmware/SDK version check before flashing if it's been a while).

- [ ] **Step 1: Build and flash**

Run: `cd ibutton_bruteforce && ufbt launch`
Expected: builds clean, uploads, launches. (Close qFlipper first — it holds
the USB port.)

- [ ] **Step 2: Fresh-seed check**

On the device (or via CLI `storage remove`), delete
`/ext/apps_data/ibutton_bruteforce/master_keys.txt` if present, then launch
**Master keys**. Expected: app starts normally (no immediate exit), and
`/ext/apps_data/ibutton_bruteforce/master_keys.txt` now exists on the SD card
with the six seed entries.

- [ ] **Step 3: Fail-closed check**

Edit `master_keys.txt` on the SD card (via CLI or card reader) to corrupt one
Dallas entry's `Data:` CRC byte. Relaunch the app.
Expected: app exits immediately; `log error` (CLI, non-interactive with a
deadline per this repo's `CLAUDE.md` log-capture rules) shows a
`master_keys self-check failed` line. Restore the file (or delete it to
re-seed) before continuing.

- [ ] **Step 4: Cyfral exhaustive check**

Settings → Protocol → Cyfral. Launch **Sequential walk**. Let it run (or set
a late Start index like 65500 to reach the end quickly). Expected: status
screen's `total` reads 65536, not 100000; the run reaches index 65535 and
reports **Done** without re-presenting earlier keys.

- [ ] **Step 5: Family round-trip check**

Settings → Family → set a non-default value (e.g. `0x02`). Back out, start
**Sequential walk** (Protocol = Dallas) briefly, then Back to stop it.
Expected: `progress.txt`'s `Family` field matches the value set; relaunching
Settings still shows the value (may show stale text until a fresh
`brute_settings_setup()` — i.e. app restart — per the known pre-existing
label-refresh behavior noted in the design doc; the persisted value itself
must be correct even if the label doesn't visually refresh mid-session).

- [ ] **Step 6: Regression check**

Master keys mode still emulates the six seed keys in order, dwell/gap timing
unchanged, Resume still round-trips — confirms Task 1/2's file-load path
didn't change master-mode behavior, only its data source.
