# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

**For general Flipper Zero FAP rules (build commands, crash/furi_check traps, log
capture, UI conventions) see `~/.claude/CLAUDE.md`. Only project-specific detail
lives here.**

# Universal Card Reader

One FAP that reads **both** card families: NFC 13.56 MHz (ISO14443-3A/3B,
ISO15693-3, FeliCa, ST25TB and everything layered on them) and LF RFID 125 kHz.
The two radios cannot run together, so it alternates timed phases and loops until
a card is found, with an animated scanning UI.

## Verified firmware / SDK — re-check before you build (STEP 0)

Last verified **2026-08-02**. Confirm these still hold (`device_info` on the
device, `api_symbols.csv` + `components.json` for the SDK) and update this block
if anything changed. Do not skip: a mismatch here caused a wedged device and a
crash that `APPCHK` did not catch.

| | Device | ufbt SDK |
|---|---|---|
| Target | `hardware_target` 7 | `hw_target` f7 |
| Firmware | `mntm-dev`, commit `42630e91`, built 31-12-2025 | official `1.4.3`, channel `release` |
| Fork | **`Momentum`** (Next-Flip/Momentum-Firmware) | Official |
| API | 87.1 | 87.1 |
| Port | COM3 (VID_0483 / PID_5740) | — |

**The API versions match exactly, so the FAP loads — but the forks are not
ABI-identical.** Known drift, both of which this app is written to survive:

- `NfcProtocol`: Momentum adds `Ntag4xx`, `Type4Tag`, `Emv` → `NfcProtocolNum`
  is 15 not 12, `NfcProtocolInvalid` is 16 not 13.
- `LFRFIDProtocol`: Momentum has 26 entries vs 24 and inserts `Indala224`
  mid-enum, shifting later ids.
- `MfClassicPollerMode`: Momentum inserts `MfClassicPollerModeDictAttackCUID`
  at id 3 → `DictAttackEnhanced` shifts. This app survives it by using read
  mode only (`MfClassicPollerModeRead`, id 0) and never requesting a dict attack.
- `MfClassicPollerEventDataKeyRequest`: Momentum inserts `key_type` before
  `key_provided`. This app survives it by never touching `key_request_data` —
  it is only used by the dict-attack modes above.

Everything else checked (`nfc_scanner.h`, `nfc_generic_event.h`, the poller
headers, `nfc_device.h`) is identical or additive-only.

## Layout — one app, in a subdirectory

```
universal_card_reader/          <- the app; run ufbt HERE, not at repo root
  application.fam              appid universal_card_reader, entry universal_card_reader_app,
                               Tools category, stack_size 12*1024
  universal_card_reader.c      app lifetime, event router, save, view callbacks
  reader_app.h                 shared types/constants; no with_view_model calls
  reader_ui.c / reader_ui.h    drawing + the only with_view_model call site
  reader_nfc.c / reader_nfc.h  protocol whitelists, NFC scan/poll/emulate
  reader_lf.c / reader_lf.h    LF RFID scan/read/emulate
  card_info.c / card_info.h    card report renderer + minimal NDEF parser
  emv.c / emv.h                read-only EMV (bank card) APDU chain
  icon.png / make_icon.py      10x10 1-bit icon, regenerate with Pillow
  README.md                    user-facing docs + the fork-ABI explanation
```

Installs to `/ext/apps/Tools/universal_card_reader.fap` (from `fap_category`),
i.e. **Apps → Tools** on the device.

The repo is `flipper-tools` — a neutral name, because it holds Flipper Zero
tooling rather than a single app. An earlier NFC-only app
(`universal_nfc_reader.c`) once lived at the root and was deleted when this one
superseded it. Nothing belongs at the root except shared tooling: `cap.py` and
its `logs/` output.

**Git repository, branch `main`.** Commits here carry a real body: what changed,
why, and an on-device **Verified** block. Build output (`dist/`,
`.vscode/compile_commands.json`) and serial captures (`logs/`) are ignored.

## Architecture

Three views + `ViewDispatcher`: the animated scan view (`ReaderViewScan`, also
used for the Notice and Emulating sub-states — see `ReaderState`), a scrollable
TextBox info view (`ReaderViewInfo`) that renders the report built by
`card_info_format_nfc()` / `card_info_format_lf()` into `app->info_text` (raw
pointer — the string must stay alive and unmodified while shown, so
`text_box_reset()` precedes every rebuild), and a Submenu actions view
(`ReaderViewActions`: Save / Emulate / Rescan / Exit), reached with **Back**
from the info view. Phase timing lives in the defines at the top of `reader_app.h`:
`NFC_PHASE_MS` 1200, `LF_PHASE_MS` 1600, `READ_TIMEOUT_MS` 2500,
`ANIM_PERIOD_MS` 80, `NOTICE_MS` 1600; per-protocol read bounds come from
`reader_read_timeout_for()`.

Every entry point that touches a radio begins with `reader_stop_all()`:
`reader_start_nfc_phase()` / `reader_start_lf_phase()` (scanning), and
`reader_start_nfc_emulation()` / `reader_start_lf_emulation()` (Emulate,
dispatched through `reader_do_emulate()`). Flow is scan → detect → poll the
most-derived safe protocol (`reader_poll_protocol()`, e.g. MfClassic /
MfUltralight / ISO14443-4A with the EMV chain) for the full card data →
render in the TextBox info screen → **Back** opens the actions menu → Save
(`reader_do_save()`, via `nfc_device_save()`/`lfrfid_dict_file_save()`) /
Emulate / Rescan / Exit.

Since the module split, `reader_ui.c` is the only file that calls
`with_view_model()`; every other module reads or writes state through its
exported wrappers (`reader_set_state()`, `reader_get_state()`,
`reader_bump_frame()`, `reader_set_notice()`).

**Five invariants that are load-bearing — breaking any of them wedges or freezes the device:**

1. **No fork-sensitive enum values.** `reader_poll_protocol()` (in `reader_nfc.c`) picks the
   most-derived pollable protocol from a compile-time whitelist (ids 0..11),
   and `dev_has()` in card_info.c guards every `nfc_device_get_data()` call —
   both via firmware-evaluated `nfc_protocol_has_parent()`. Never reintroduce
   `NfcProtocolInvalid` or `NfcProtocolNum`; the verified device (Momentum)
   numbers them differently from the SDK we compile against.
   `reader_emulatable_protocols` (also in `reader_nfc.c`) is the same idea applied to emulation: only
   protocols whose *entire* ancestor chain has a non-NULL entry in the
   firmware's `nfc_listeners_api[]` may reach `nfc_listener_alloc()` — that
   table is walked with no NULL check at all beyond the leaf protocol, so an
   unlisted ancestor is an unchecked crash, not even a `furi_check`.
2. **Generation-stamped events.** `EVENT_MAKE(id, gen)` / `EVENT_ID` / `EVENT_GEN`
   pack `app->gen` into the high bits; `app->gen++` on every phase change and the
   handler drops mismatches. **Keep every `ReaderCustomEvent` value below 256** —
   the upper bits are the generation. `ReaderEventAnimTick` is exempt from the check.
3. **The LF phase owns the LF worker thread.** `lfrfid_worker_start_thread()` is
   in `reader_start_lf_phase()` / `reader_start_lf_emulation()`, not in
   `reader_app_alloc()`, and `reader_stop_lf()` calls
   `lfrfid_worker_stop_thread()` to *join* before NFC may start.
   `lfrfid_worker_stop()` alone does not wait. `reader_start_lf_phase()`,
   `reader_start_lf_emulation()`, and `reader_stop_lf()` all live in `reader_lf.c`.
4. **Back is owned by the ViewDispatcher's navigation callback**, never by a
   module view's `previous_callback`. `view_previous()` passes `view->context`,
   and `text_box_alloc()`/`submenu_alloc()` set that context to the
   `TextBox`/`Submenu` itself — a `view_set_previous_callback()` on either view
   is handed that pointer to use as a `ReaderApp*`. That was a real crash (see
   below); the dispatcher's navigation callback gets `event_context`, which
   *is* the app. `reader_switch_view()` is the only place that changes views:
   it records `app->current_view` (the navigation callback reads it to choose
   menu-open / menu-close / exit) and starts/stops the animation timer, which
   only the scan view needs (Emulating reuses it for the radar; Notice stops
   it again right after entry, since it renders nothing animated).
5. **`reader_phase_timer_callback()` never touches the view model.** It runs
   on the TimersSrv thread, not the GUI thread; calling `with_view_model()`
   from there deadlocked against `furi_timer_start()` on the GUI thread (both
   funnel through the same FreeRTOS timer command queue) and froze the app
   the moment *any* phase timer fired — the ordinary NFC/LF phase timeout,
   not just the Notice path. Read a plain `ReaderApp` field instead
   (`app->notice_active`), the same way `app->gen` / `app->lf_phase` are
   already read cross-thread elsewhere in this app — see below.

`protocol_dict_alloc(lfrfid_protocols, LFRFIDProtocolMax)` passes our
compile-time count against the firmware's array. Safe when the fork has more
protocols (they are simply not detected); LF ids always round-trip through the
firmware's own array, so names stay correct.

## Testing this app

Verified device: Momentum `mntm-dev`, API 87.1, **COM3**. Helper script:
`cap.py` at the repo root — a pyserial capture with a hard deadline,
`--cmd`/`--cmd-delay` for pre-capture CLI commands (repeatable) so `log`
attaches immediately after, `--deadline` for the capture window, `--out` to
name the transcript (`python cap.py --cmd "log debug" --deadline 15
--out logs/cap_x.log`).

**Every capture transcript goes to `logs/` — never write a log at the repo
root.** `cap.py` enforces it rather than relying on the caller: with no `--out`
it writes `logs/cap_<timestamp>.log`, a bare `--out cap_x.log` is redirected to
`logs/cap_x.log`, and only an `--out` carrying a path separator is taken
verbatim. It prints the resolved path to stderr before opening the port.
`logs/` and `*.log` are gitignored, so captures are never committed.

- With an NFC card on the antenna the NFC phase wins in ~300 ms every time, so
  **the LF path is only exercised with the card removed.** Confirm alternation by
  capturing ~25 s at `log debug` and checking `phase: LF (gen N)` /
  `phase: NFC (gen N+1)` alternate, then that `top` still answers.
- `input send back short` on the result screen now opens the actions menu
  instead of rescanning directly (Save/Emulate/Rescan/Exit — see
  Architecture). To catch a fresh read's logs non-interactively: `back short`
  (open menu), `down short` x2 + `ok short` (Save=0/Emulate=1/**Rescan=2**),
  then `log debug` — **a `log` session accepts no further commands**, so
  inputs must be injected first.
- A crash is visible from the host as the USB VCP dropping mid-command
  (`ClearCommError failed`) plus a reset `uptime`; `furi_crash` output never
  reaches USB, only the screen and the UART console. `uptime` also refuses to
  answer while an app is open, which makes it a free liveness probe.
- `top` should show `LfrfidWorker` **only** during the LF phase and
  `NfcScanWorker`/`NfcWorker` only during the NFC phase. Overlap means invariant 3
  is broken.

Status logs are `FURI_LOG_I` (detection, read with UID/ID hex, read timeout);
phase changes and stale-event drops are `FURI_LOG_D`, so capture at `debug` when
debugging phase logic.

**Known:** the ISO14443-4A test card has a random UID (first byte `0x08`), so
its UID legitimately differs on every read. **Still not verified on device:**
a real 125 kHz LF read / LF Save/Emulate (needs an LF card presented with
*no* NFC card anywhere near the antenna — confirmed live on 2026-08-02 that an
NFC card left nearby gets re-detected and re-read within ~300 ms of every
relaunch/Rescan, so LF never gets a turn while one lingers), and the
payment-card Save/Emulate policy's actual *block* path (needs a card whose
PPSE select returns `9000` with ≥1 AID — every ISO14443-4A-family card seen
so far returned `6A86`/`0000`, i.e. `ppse_ok=false`, so `reader_is_payment_card()`
correctly did not flag them; the block branch itself is implemented and
source-reviewed but has not fired on a real card). Test both per the updated
bullets above before relying on them.

**Verified 2026-08-02 — NFC Save/Emulate/Rescan/Exit, on device, with a real
Mifare Classic 1K card.** `back short` → `ok short` (Save, default selection)
wrote `/ext/nfc/Mifare_Classic_<UID>.nfc`; `storage read` confirmed a
well-formed FlipperFormat file (`Device type: Mifare Classic`, full block
dump, correct UID/ATQA/SAK) that overwrites cleanly on a repeat save. `down
short` + `ok short` (Emulate) brought up `NfcWorker` with `NfcScanWorker`/
`LfrfidWorker` both absent, confirming `nfc_listeners_api[NfcProtocolMfClassic]`
really is non-NULL on this firmware; **Back** released it (thread count back to
baseline) and returned to the report. Rescan and Exit (`uptime` refused →
answered again after Exit, proving a clean `view_dispatcher_stop()`) both
confirmed too. `Stack Min` for the app thread stayed at 11156–11184 of 12284
bytes through all of it — comfortable margin.

**Verified 2026-08-02 — same, on two more real cards, different protocol.**
A card the scanner classified `ISO14443-4A` and another `Mifare Plus` (both
resolve to `poll_protocol = NfcProtocolIso14443_4a`, per `reader_poll_protocol()`)
each went through Save + Emulate cleanly (`NfcWorker` up, `NfcScanWorker`/
`LfrfidWorker` absent, clean Back-teardown, no `furi_check`). Both cards'
`emv_read()` PPSE select failed (`sw=6A86` and `sw=0000`, `aids=0`), so
neither triggered the payment-card policy — but both are worth noting as a
concrete real-world confirmation of the *structural* half of that guarantee
regardless: the saved file's `Device type:` is `ISO14443-4A` with only
UID/ATQA/SAK/ATS fields even though the on-screen `Type:`/filename said
`Mifare Plus` (from `display_protocol`) — `EmvData` never touches
`nfc_device_save()`'s input no matter what the scanner calls the card or
whether `emv_read()` found anything. (Note: `SAK: 20` on both — a common
signature for real contactless-payment silicon — so these may well be bank
cards whose PPSE just didn't complete over this reader; if so, the fact they
still saved/emulated as bare transport with zero PAN/AID/name fields is
exactly the intended fallback, not a policy gap: nothing sensitive was ever
captured to leak in the first place.)

**Fixed 2026-08-02 — Back on the result screen rebooted the device.**
`view_set_previous_callback(text_box_get_view(...), cb)` had the callback cast
`view->context` to `ReaderApp*`, but that context is the `TextBox` (8-byte
struct), so `app->phase_timer` / `app->poller` were read out of a neighbouring
heap block and passed to `furi_timer_stop()` / `nfc_poller_stop()`. Proven on
device by logging both pointers from a non-dereferencing callback:
`prev_ctx=2000A578 app=2000A5C0 text_box=2000A578`. Replaced by
`view_dispatcher_set_navigation_event_callback()` — see invariant 4.

**Fixed 2026-08-02 — adding Save/Emulate's notice screen froze the app on the
very first phase timeout.** `reader_phase_timer_callback()` (already running on
the TimersSrv thread, not the GUI thread) was changed to call
`with_view_model(app->view, ..., false)` to decide Notice-dismiss vs.
phase-timeout. That deadlocked against `furi_timer_start()` on the GUI thread:
confirmed on device by `top` showing the app thread `Blocked` at an unchanging
`Stack Min` and zero `NfcScanWorker`/`LfrfidWorker` threads for 20+ seconds
after launch, with zero `phase:` log lines even at `log debug`. Fixed by
reading a plain field (`app->notice_active`) instead of the model — see
invariant 5.

---

# RFID Multi-Reader

Second FAP in this repo, `rfid_multi_reader/`. Universal **read-only** RFID
reader: 125 kHz LF, 13.56 MHz HF/NFC (both built-in), plus a UHF (860-960 MHz)
menu row that explains an external module is required. No save, no
emulation - read + display only. Radio work sits behind a plugin vtable
(`RfidBackend`, in `rfid_backend.h`) so a future UHF-over-GPIO implementation
is a new `backend_uhf.c` body with zero core changes.

**`universal_card_reader/` is not touched by this app.** Its proven NFC
extraction, phase-alternation and crash-avoidance patterns were ported into
`rfid_multi_reader`'s backends; its raw-hex-only LF output was replaced with
decoded fields via `protocol_dict_render_data()`.

## Verified firmware / SDK — re-check before you build (STEP 0)

Last verified **2026-08-02**, live on device (`device_info` over the CLI,
re-read against `api_symbols.csv`/`components.json`). Same device and SDK as
Universal Card Reader above — see that table for the fork-ABI drift detail.
Re-run `device_info` before trusting this if the device may have been
reflashed since.

| | Device | ufbt SDK |
|---|---|---|
| Target | `hardware_target` 7 | `hw_target` f7 |
| Firmware | `mntm-dev`, commit `42630e91`, built 31-12-2025 | official `1.4.3`, channel `release` |
| Fork | `Momentum` (Next-Flip/Momentum-Firmware) | Official |
| API | 87.1 | 87.1 |
| Port | COM4 this session (was COM3 for Universal Card Reader — Windows reassigns the port on reconnect; not a device change) | — |

## Layout

```
rfid_multi_reader/              <- the app; run ufbt HERE, not at repo root
  application.fam               appid rfid_multi_reader, entry rfid_multi_reader_app,
                                Tools category, stack_size 8*1024
  rfid_backend.h                the whole plugin contract (RfidBackend vtable)
  rfid_app.h                    shared types/constants; no NFC/LFRFID header (keeps
                                the core decoupled from radio specifics)
  rfid_multi_reader.c           app lifetime, event router, menu, phase/rotation logic
  ui.c / ui.h                   drawing + the only with_view_model call site
  backend_hf.c / .h             13.56 MHz HF/NFC: scan/poll/describe + PPSE payment probe
  backend_lf.c / .h             125 kHz LF: scan/read/describe with decoded fields
  backend_uhf.c / .h            stub: available() = false, "external module" notice
  icon.png / make_icon.py       10x10 1-bit icon (emitter + waves), regenerate with Pillow
```

Installs to `/ext/apps/Tools/rfid_multi_reader.fap`, i.e. **Apps → Tools**.

## Architecture

Three views + `ViewDispatcher`: a Submenu band menu (`RfidViewMenu`, the
start view), a custom animated status View (`RfidViewStatus`, reused for
Scanning/Reading/Notice sub-states via `RfidState`), and a TextBox info view
(`RfidViewInfo`). No actions submenu (no Save/Emulate/Rescan - read-only): Back
on the report goes straight to the band menu, and re-picking the same row is
the rescan.

`rfid_start_scan()`/`rfid_advance_phase()` in `rfid_multi_reader.c` drive an
`app->rotation[]` array built per mode by `rfid_build_rotation()` (Auto = HF
then LF, in that order; HF-only/LF-only = one entry; UHF is never in a
rotation - `RfidEventMenuUhf` goes straight to `rfid_show_notice()` and never
touches `app->mode`). `rfid_stop_all()` calls every registered backend's
`scan_stop()`, not just the active one - the one guarantee that two radios
can never be up at once. Timing: `HF_PHASE_MS` 1200, `LF_PHASE_MS` 1600,
`ANIM_PERIOD_MS` 80, `NOTICE_MS` 1800 (`rfid_app.h`); per-backend read bounds
come from each backend's `read_timeout_ms()`.

Same five invariants as Universal Card Reader apply here (fork-sensitive
enums, generation-stamped events, LF-worker-thread ownership/join, Back owned
by the ViewDispatcher navigation callback not a `previous_callback`, timer
callback never touching the view model) - see that app's section above for
the full reasoning; `rfid_app.h`'s comments carry the short form. One more,
specific to the vtable split: **`RfidEventDetected`'s state guard
(`if(app->state != RfidStateScanning) return true;`) is load-bearing**,
the same way the gen check is - the NFC scanner re-detects in a loop, and a
duplicate Detected posted *after* `app->gen++` still passes the gen check.

## Testing this app

Same device/port/`cap.py` mechanics as Universal Card Reader above. Two
CLI behaviours worth knowing, confirmed this session:

- **`top` and `log debug` are both live, continuously-refreshing streams**
  that do not return to the prompt until `Ctrl+C` (`\x03`) - any `--cmd` sent
  to `cap.py` *after* one of them in the same invocation is not processed as
  a command. Snapshot `top` with `--cmd "top" --cmd $'\x03'` (short
  `--cmd-delay`) to capture one refresh and stop. To catch a log line fired
  synchronously by an input press (e.g. a single-band `phase:` line), start
  `log debug` *before* the input in a fresh capture and accept you may still
  miss a one-shot line fired within the first fraction of a second; a
  repeating line (Auto-mode phase alternation) is far more reliable to catch
  this way.
- **`loader close` force-closes the running app from any UI state**,
  including one input navigation left in an unexpected place (e.g. bounced
  between two views by miscounted Back presses). Prefer it over chained
  `input send back short` for test cleanup/recovery.
- `top`'s thread list does **not** show a distinctly-named NFC/LF worker
  thread on this firmware even while a scan is genuinely active and reading
  a real card (confirmed by comparison against `universal_card_reader` mid
  read) - unlike that app's own testing notes, absence of such a thread in
  `top` is not by itself evidence a scan failed to start. Use the state
  machine instead: in single-band mode (`rotation_len == 1`, no phase timer)
  a lone `Back` returns to the menu if scanning is genuinely in progress, or
  exits the app outright if a notice had already auto-dismissed - a quick,
  reliable way to tell the two apart remotely.

**Verified 2026-08-02, on device, with a real payment (EMV/ISO14443-4A)
card** - the only physical RFID object available in this session: full
scan → detect → read → describe → report pipeline, `HF only` and `Auto`
(Auto's first phase is HF and wins detection in ~150-300 ms, well inside
`HF_PHASE_MS`, so Auto could not be observed reaching LF this session - see
below). Report contained correct `Band`/`Type: EMV`/`UID`/`ATQA`/`SAK`/
`ATS hist`/`Frame max`, and **`Payment: EMV application present` fired** -
the PPSE probe succeeded where Universal Card Reader's fuller EMV chain
never did on any card tested in this repo. `Chain: ISO14443-4A <- ISO14443-3A`
confirmed the fork-only-id degradation design works exactly as intended:
Momentum's `Emv` protocol id (14, not in the SDK enum this app compiles
against) is simply absent from `hf_chain_order[]`, yet `Type:` still shows
the firmware's own name and the chain still resolves through
`nfc_protocol_has_parent()`. `Stack Min` for the app thread was 7424/8188
bytes after a full read+report cycle - comfortable headroom, no need to
raise `stack_size` past `8 * 1024`. Menu navigation (all 4 rows), the UHF
notice (auto-dismiss timing confirmed via the single-Back state-machine
trick above), and clean exit (`uptime` refused while open, answered again
after Back x2, uptime kept climbing across the whole session - no resets)
all confirmed too.

**Not verified on device (no such card was available this session):** LF
decoded-field rendering (`lf_scan_start()`/`lfrfid_worker` engagement *is*
confirmed live - firmware log `[D][LfRfidWorker] Read started` - but no
125 kHz card was present to reach `protocol_dict_render_data()`); the
HF sections for ISO14443-3B, ISO15693-3, FeliCa, ST25TB, MfUltralight and
MfClassic (code-reviewed against the SDK headers and ported from
`card_info.c`'s hardware-proven renderers, but not exercised - no such cards
were available); Auto-mode's phase-timeout → `rfid_advance_phase()` →
LF-start transition specifically (needs the antenna clear of any HF card,
which this session's fixed physical card made impossible; the two halves it
connects - HF `scan_start`/`scan_stop` and LF `scan_start`/`scan_stop` - are
each independently confirmed working). Test all of the above with the
matching cards, and Auto alternation with the antenna clear, before relying
on them.

---

# context-mode — MANDATORY routing rules

You have context-mode MCP tools available. These rules are NOT optional — they protect your context window from flooding. A single unrouted command can dump 56 KB into context and waste the entire session.

## BLOCKED commands — do NOT attempt these

### curl / wget — BLOCKED
Any Bash command containing `curl` or `wget` is intercepted and replaced with an error message. Do NOT retry.
Instead use:
- `ctx_fetch_and_index(url, source)` to fetch and index web pages
- `ctx_execute(language: "javascript", code: "const r = await fetch(...)")` to run HTTP calls in sandbox

### Inline HTTP — BLOCKED
Any Bash command containing `fetch('http`, `requests.get(`, `requests.post(`, `http.get(`, or `http.request(` is intercepted and replaced with an error message. Do NOT retry with Bash.
Instead use:
- `ctx_execute(language, code)` to run HTTP calls in sandbox — only stdout enters context

### WebFetch — BLOCKED
WebFetch calls are denied entirely. The URL is extracted and you are told to use `ctx_fetch_and_index` instead.
Instead use:
- `ctx_fetch_and_index(url, source)` then `ctx_search(queries)` to query the indexed content

## REDIRECTED tools — use sandbox equivalents

### Bash (>20 lines output)
Bash is ONLY for: `git`, `mkdir`, `rm`, `mv`, `cd`, `ls`, `npm install`, `pip install`, and other short-output commands.
For everything else, use:
- `ctx_batch_execute(commands, queries)` — run multiple commands + search in ONE call
- `ctx_execute(language: "shell", code: "...")` — run in sandbox, only stdout enters context

### Read (for analysis)
If you are reading a file to **Edit** it → Read is correct (Edit needs content in context).
If you are reading to **analyze, explore, or summarize** → use `ctx_execute_file(path, language, code)` instead. Only your printed summary enters context. The raw file content stays in the sandbox.

### Grep (large results)
Grep results can flood context. Use `ctx_execute(language: "shell", code: "grep ...")` to run searches in sandbox. Only your printed summary enters context.

## Tool selection hierarchy

1. **GATHER**: `ctx_batch_execute(commands, queries)` — Primary tool. Runs all commands, auto-indexes output, returns search results. ONE call replaces 30+ individual calls.
2. **FOLLOW-UP**: `ctx_search(queries: ["q1", "q2", ...])` — Query indexed content. Pass ALL questions as array in ONE call.
3. **PROCESSING**: `ctx_execute(language, code)` | `ctx_execute_file(path, language, code)` — Sandbox execution. Only stdout enters context.
4. **WEB**: `ctx_fetch_and_index(url, source)` then `ctx_search(queries)` — Fetch, chunk, index, query. Raw HTML never enters context.
5. **INDEX**: `ctx_index(content, source)` — Store content in FTS5 knowledge base for later search.

## Subagent routing

When spawning subagents (Agent/Task tool), the routing block is automatically injected into their prompt. Bash-type subagents are upgraded to general-purpose so they have access to MCP tools. You do NOT need to manually instruct subagents about context-mode.

## Output constraints

- Keep responses under 500 words.
- Write artifacts (code, configs, PRDs) to FILES — never return them as inline text. Return only: file path + 1-line description.
- When indexing content, use descriptive source labels so others can `ctx_search(source: "label")` later.

## ctx commands

| Command | Action |
|---------|--------|
| `ctx stats` | Call the `ctx_stats` MCP tool and display the full output verbatim |
| `ctx doctor` | Call the `ctx_doctor` MCP tool, run the returned shell command, display as checklist |
| `ctx upgrade` | Call the `ctx_upgrade` MCP tool, run the returned shell command, display as checklist |
