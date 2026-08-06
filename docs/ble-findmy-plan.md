# BLE Find My Module — Implementation Plan

## Goal
Add a `ble_findmy` module to `universal_toolkit` that broadcasts an Apple Find My BLE beacon carrying a static OpenHaystack SECP224R1 public key. The beacon persists after module exit (documented deviation from the Phase 0 lifecycle contract: BLE core2 additive exception).

## Files to create/modify

### 1. `universal_toolkit/toolkit_app.h` — add view base
- Append `#define TOOLKIT_VIEW_BASE_BLE_FINDMY 0x50u` after `TOOLKIT_VIEW_BASE_SUBGHZ_REC 0x40u`
- `ToolkitSubsysBle` already exists (value 6), no enum change needed

### 2. `universal_toolkit/modules/ble_findmy.h` — new file
```c
#pragma once
#include "../toolkit_app.h"

void ble_findmy_enter(ToolkitApp* app);
void ble_findmy_exit(ToolkitApp* app);
bool ble_findmy_event(ToolkitApp* app, uint32_t id);
bool ble_findmy_nav(ToolkitApp* app);
```

### 3. `universal_toolkit/modules/ble_findmy.c` — new file

**Architecture:**
- Phase 0 module pattern (like `gpio_info.c`): `View*` + `FuriTimer*` in ctx, draw callback, timer callback, four lifecycle callbacks
- No model needed — the view is static (shows key status + beacon state)
- Timer at 500 ms polls `furi_hal_bt_extra_beacon_is_active()` to update display

**Key constants:**
```c
#define BLE_FINDMY_REFRESH_EVENT 0u
#define BLE_FINDMY_REFRESH_MS    500
```

**Context struct:**
```c
typedef struct {
    View* view;
    FuriTimer* refresh_timer;
    bool beacon_active;
} BleFindMyCtx;
```

**Draw callback:**
- Title bar: "BLE Find My"
- Shows the public key (first 16 bytes hex, then next 12 bytes hex on second line)
- Shows beacon state: "Beacon: ON" or "Beacon: OFF"
- Shows "Back: exit module" at bottom

**Timer callback:**
- Posts `EVENT_MAKE(BLE_FINDMY_REFRESH_EVENT, app->gen)` — runs on TimersSrv, never touches view model

**`ble_findmy_enter()`:**
1. Alloc `BleFindMyCtx`, store as `app->active_ctx`
2. Alloc view, set draw callback
3. Add view at `TOOLKIT_VIEW_BASE_BLE_FINDMY`
4. Ensure C2 is in stack mode: `furi_hal_bt_ensure_c2_mode(BleGlueC2ModeStack)`
5. Configure extra beacon:
   - `GapExtraBeaconConfig` with:
     - `min_adv_interval_ms = 2000, max_adv_interval_ms = 2000` (Apple Find My spec: ~2 s)
     - `adv_channel_map = GapAdvChannelMapAll`
     - `adv_power_level = GapAdvPowerLevel_0dBm`
     - `address_type = GapAddressTypeRandom`
     - `address[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06}` (static random)
   - Call `furi_hal_bt_extra_beacon_set_config(&config)`
6. Set beacon data: 28-byte OpenHaystack advertisement (static key)
   - `furi_hal_bt_extra_beacon_set_data(data, 28)`
7. Start beacon: `furi_hal_bt_extra_beacon_start()`
8. Alloc + start periodic timer
9. Switch to view
10. Log entry: `ToolkitSubsysBle`, summary `"ble findmy beacon started"`

**`ble_findmy_exit()`:**
1. Stop + free timer
2. **Do NOT stop the beacon** — documented deviation: beacon persists after module exit
3. Remove view, free view, free ctx, NULL `app->active_ctx`

**`ble_findmy_event()`:**
- On `BLE_FINDMY_REFRESH_EVENT`: poll `furi_hal_bt_extra_beacon_is_active()`, update ctx, redraw
- Return `true` (handled)

**`ble_findmy_nav()`:**
- `toolkit_exit_module(app)` — return `true`

**Static key (28 bytes):**
```c
static const uint8_t ble_findmy_key[28] = {
    // OpenHaystack SECP224R1 public key (example — user replaces with their own)
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b,
};
```

### 4. `universal_toolkit/toolkit.c` — register module
Append to `modules[]` table after SubGHz Recorder:
```c
{
    .name = "BLE Find My",
    .view_base = TOOLKIT_VIEW_BASE_BLE_FINDMY,
    .enter = ble_findmy_enter,
    .exit = ble_findmy_exit,
    .event = ble_findmy_event,
    .nav = ble_findmy_nav,
},
```
Add `#include "modules/ble_findmy.h"` at top.

### 5. `universal_toolkit/application.fam` — no change needed
Stack size 16*1024 is already sufficient. No new SDK dependencies — all `furi_hal_bt_extra_beacon_*` functions are already linkable.

## API surface verified
All called functions confirmed `Function,+` in `api_symbols.csv`:
- `furi_hal_bt_extra_beacon_set_config`
- `furi_hal_bt_extra_beacon_set_data`
- `furi_hal_bt_extra_beacon_start`
- `furi_hal_bt_extra_beacon_stop`
- `furi_hal_bt_extra_beacon_is_active`
- `furi_hal_bt_ensure_c2_mode`
- `BleGlueC2ModeStack` (enum value 2)

## Deviation from lifecycle contract
The Phase 0 contract says `exit` releases the peripheral. This module deliberately does NOT stop the extra beacon in `exit` — the beacon persists so the Flipper remains trackable via Apple's Find My network even after the user leaves the module. This is a documented BLE core2 additive exception: the extra beacon runs on core2 independently of the FAP, and stopping it would defeat the purpose. The user must power-cycle or use a future "stop beacon" action to disable it.

## Verification
1. `ufbt` from `universal_toolkit/` — must compile clean
2. `ufbt launch` — app loads, launcher shows "BLE Find My" row
3. Enter module → beacon starts → display shows "Beacon: ON"
4. Back → returns to launcher → beacon still active (verify via another BLE scanner)
5. Re-enter module → display still shows "Beacon: ON"
6. Back → Back → clean app exit, beacon still active
