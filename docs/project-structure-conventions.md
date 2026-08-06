# Project Structure & Conventions

The single source of truth for how this repo is laid out and how any agent
(Claude Code or other) must place code.

## Clean Architecture, adapted to Flipper

Three layers; **dependencies point inward only** — inner never includes outer.

- **Domain (pure C, no Flipper/HAL includes).** Data + algorithms that can
  compile and be tested on a host: the Find My advertisement byte layout,
  key hex→bytes parsing, the log-record model, Sub-GHz decode math, etc. No
  `furi_*`, no `gui`, no `storage`. This is what makes the tool testable.
- **Application / module.** The `ToolkitModule` lifecycle, views, event
  handling, orchestration. Depends on Domain + the module contract. One
  module = one folder.
- **Infrastructure / HAL adapters.** `furi_hal_bt`, `storage`, `gui`,
  `view_dispatcher`. Outermost; called by the module layer, never by Domain.

The `ToolkitModule` descriptor is the port/interface: the core dispatches
through it, modules plug into it. Core depends on the contract, not on any
concrete module; modules depend on core + domain, not on each other.

## Target layout

```
flipper-tools/
  CLAUDE.md                 # conventions + invariants (this doc's rules live here or link here)
  README.md  cap.py  .gitignore
  docs/                     # design docs, plans, reviews
  apps/                     # standalone FAPs — ONE canonical copy each, independently buildable
    universal_card_reader/
    subghz_auto_recorder/
    rfid_multi_reader/
  lib/                      # shared, HAL-light domain code reused by both a standalone app
                            # AND its toolkit module
  universal_toolkit/
    application.fam
    icon.png  make_icon.py
    toolkit.c               # CORE: launcher, dispatch, lifecycle, module table
    toolkit_app.h           # CORE: app struct, ToolkitModule contract, enums, log types, macros
    toolkit_log.{c,h}       # CORE: spine record writer
    toolkit_ui.{c,h}        # CORE: shared chrome (title bar, notice) only
    modules/
      gpio_info/
        gpio_info.{c,h}
      ble_findmy/
        ble_findmy.{c,h}       # module layer: views, lifecycle, furi_hal_bt calls
        findmy_payload.{c,h}   # DOMAIN: pubkey -> adv bytes; zero Flipper deps
```

## Rules for agents

1. **One module = one folder** under `universal_toolkit/modules/<name>/`.
   Never drop module files flat in `modules/`.
2. **Domain code has no Flipper includes.** If a file needs `furi_*`/`gui`/
   `storage`, it is module or infra, not domain.
3. **Single source of truth — never copy an app or module.** A standalone app
   lives once in `apps/`. When a subsystem is wrapped as a toolkit module, its
   reusable logic is extracted into `lib/` and consumed by **both** the
   standalone app and the module.
4. **The module contract is the only integration point.** New subsystem → new
   `ToolkitModule` entry + its folder; register/unregister its view-id block on
   enter/exit; one `with_view_model` site per module; gen-stamped events
   (< 256, module-local); the corrected exit order (`show_launcher → teardown
   → gen++`); BLE-persist is the one documented release-on-exit exception.
5. **Build via glob.** `application.fam` `sources` uses `modules/**/*.c` so a
   new module folder needs no manifest edit. Keep module-internal includes
   relative.
6. **Carry the existing invariants** (no fork-shifting enum / no private decoder
   structs; `storage_simply_remove` checked; magic numbers as named `#define`s;
   deliberate copy-paste of *specialised* draw code stays per-module).
7. **Review + git unchanged:** `flipper-c-review` → `flipper-perf-review`;
   explicit-path staging; commit message to `/tmp`; omit the `Refs` trailer.
8. **Docs live in `docs/`.** Plans, reviews, and this file.

## Current status / blockers

- `apps/` is not yet populated. The three existing apps (`universal_card_reader`,
  `subghz_auto_recorder`, `rfid_multi_reader`) currently live under
  `universal_toolkit/` and are compiled into the toolkit because their module
  wrappers depend on the apps' internal app structs. Moving them to `apps/`
  and making them standalone again requires **Phase 1** extraction of the
  reusable core into `lib/` so the toolkit module can link `lib/` instead of
  the whole app. Until that happens, the apps remain in `universal_toolkit/`
  so the toolkit build stays clean.
