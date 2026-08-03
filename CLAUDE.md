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

Last verified **2026-08-02**, re-checked after a firmware update: `device_info`
read live off the device, and the fork-ABI drift below re-confirmed against
Momentum's actual header sources at the new commit (not just assumed stable
because the API number didn't move). Update this block again if anything
changes. Do not skip: a mismatch here caused a wedged device and a crash that
`APPCHK` did not catch.

| | Device | ufbt SDK |
|---|---|---|
| Target | `hardware_target` 7 | `hw_target` f7 |
| Firmware | `mntm-dev`, commit `8ed809fb`, built 03-06-2026 | official `1.4.3`, channel `release` |
| Fork | **`Momentum`** (Next-Flip/Momentum-Firmware) | Official |
| API | 87.1 | 87.1 |
| Port | COM3 last confirmed (VID_0483 / PID_5740) — re-enumerate before trusting; not re-checked this session | — |

**The API versions match exactly, so the FAP loads — but the forks are not
ABI-identical.** Known drift, confirmed still present in Momentum's source at
commit `8ed809fb` (2026-06-02 build) — this app is written to survive all four:

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

**Six invariants that are load-bearing — breaking any of them wedges or freezes the device:**

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
6. **Any blocking modal call must stop `anim_timer` first.**
   `view_dispatcher_send_custom_event()` blocks `FuriWaitForever` on a
   16-deep queue; an 80 ms tick left running while the GUI thread is blocked
   inside a modal (`dialog_file_browser_show()`, today's only example) fills
   that queue in ~1.3 s and then blocks the TimersSrv thread for the whole
   modal. `reader_do_load()` calls `furi_timer_stop(app->anim_timer)` before
   `dialog_file_browser_show()` for exactly this reason — never add a second
   blocking dialog call without the same guard, and never move the stop
   after the call.

`protocol_dict_alloc(lfrfid_protocols, LFRFIDProtocolMax)` passes our
compile-time count against the firmware's array. Safe when the fork has more
protocols (they are simply not detected); LF ids always round-trip through the
firmware's own array, so names stay correct.

## Testing this app

Verified device: Momentum `mntm-dev`, API 87.1, **COM3** (seen as **COM4**
mid-session after a reboot — Windows reassigns the port on reconnect, not a
device change; re-enumerate with `python -c "import serial.tools.list_ports
as lp; [print(p.device, p.hwid) for p in lp.comports()]"` if a capture can't
open the port). Helper script:
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
- **`top` and `log <level>` are both live, continuously-refreshing streams
  that do not return to the prompt.** Any `--cmd` queued after one of them in
  the same `cap.py` invocation is silently dropped, not executed — send
  every triggering input *before* attaching `log`/`top`, never after. To
  confirm an action fired, prefer a filesystem/thread-list oracle
  (`storage list`, `top`'s thread table) over trying to catch the log line
  live.
- **`loader close` is not always a reliable recovery.** It force-closes an
  app that is merely on an unexpected *view* (confirmed working for ordinary
  navigation mix-ups), but it cannot interrupt a GUI thread genuinely
  blocked inside a synchronous SDK call — confirmed live 2026-08-02 (see the
  Emulate hang below): `loader close` reported success repeatedly while
  `loader info` kept showing the app running. `power reboot` over the CLI
  recovers even that case (device re-enumerates in ~5 s) without needing a
  physical button-combo reset.
- **Load's file browser is scoped to `READER_SAVE_DIR`** — confirmed via
  `top`: `dialogs BrowserWorker`'s `Stack Min` only grows once the browser
  has actually opened, a reliable non-visual signal the dialog is live
  before selecting anything (useful generally: there is no way to read the
  screen over the CLI).
- **`dialog_file_browser_show()`'s file-pick (`ok`) cannot be driven over the
  CLI** — confirmed exhaustively 2026-08-03: bare `ok short`, the documented
  `ok press` / `ok short` / `ok release` triple, `ok long`, and `right short`
  all leave `dialogs BrowserWorker` running with `Stack Min` unchanged (no
  pick registers), while the identical sequences work fine for Submenu items
  and for `back` (cancels the dialog cleanly either way). Script navigation
  up to opening the browser, then a physical button press is required to
  actually pick a file. Rapid-fire bare-`short` bursts against this dialog
  produced two crash-reboots and one `dialog_file_browser_show()` hang that
  `loader close` could not clear (see above) — `power reboot` recovered
  both. The documented protocol is three separate commands, `input send
  <key> press` / `<key> short` / `<key> release`
  (docs.flipper.net/zero/development/cli); bare `short` happens to work for
  Submenu navigation and usually for `back`, but is not the real contract.

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

**Verified 2026-08-02 — Save retargeted to the app data folder, Load added.**
`back short` → `ok short` (Save) on a live-read EMV/payment card wrote
`/ext/apps_data/universal_card_reader/EMV_<UID>.emv`; `storage list
/ext/nfc` before and after was byte-for-byte the same set of files — nothing
in the old shared tree moved or was touched. `storage list /ext/apps_data`
confirms the app-data convention matches a dozen other installed apps
(`nfc`, `subghz`, `metroflip`, …) already using that same
`/ext/apps_data/<appid>` root.

Load (via the Actions menu and the scan-screen **OK** shortcut) opens the
browser scoped to that folder (`dialogs BrowserWorker`'s `Stack Min` growing
from its 1092-byte idle baseline to 1360–1392 is the non-visual "the dialog
is actually open" signal used throughout this session, since there is no
way to read the screen over the CLI) and round-trips a saved `.nfc` file:
loaded a Mifare Classic dump, deleted it from disk, pressed Save, and the
identical file reappeared — proof the load → `app->device`/`app->card` →
save pipeline is intact end to end. Cancelling the browser (**Back**) with
no selection correctly returns to the report (confirmed by a second
**Back** opening the actions menu rather than exiting the app — exiting is
what a stray Back on the *scan* screen does, so surviving it proves Load's
cancel path went to `ReaderViewInfo`, matching `from_scan == false` for an
Actions-menu trigger). A `.emv` file whose FlipperFormat header does not say
`Universal EMV Card`/version 2 (a real `.nfc` file copied over a `.emv`
extension) is rejected by the hardened `emv_load()` header check without a
crash — `top` stayed clean (stable `Stack Min`, no stray threads) through
the whole attempt. `Stack Min` for the app thread ranged 11080–11216 of
12284 bytes across every Save/Load cycle this session — comfortable margin,
same ballpark as the pre-Load baseline above.

**Not independently isolated this session:** the scan-screen-specific
anim-timer hazard (invariant 6) needs Load triggered *from `ReaderViewScan`*
with `anim_timer` still ticking, held open ≥10 s. The live EMV/payment card
used throughout this session never left the antenna, so every Rescan
re-detected and re-read it within roughly 1–2 s — far too narrow a window
to land a scripted `input send` inside reliably (same constraint already
noted above for LF). The fix itself was re-confirmed by inspection
(`furi_timer_stop(app->anim_timer)` unconditionally precedes
`dialog_file_browser_show()` in `reader_do_load()`) and indirectly
corroborated: this session held that same dialog open for double-digit
seconds several times via the Actions-menu path with no TimersSrv-blocked
symptom. Isolate the scan-screen case specifically with the antenna clear,
the same way LF needs it.

**Found and fixed 2026-08-02 — `reader_do_load()`'s `.nfc` branch set
`poll_protocol` directly from `display_protocol`.** A live read sets
`poll_protocol = reader_poll_protocol(best)`, which walks a protocol up to
the nearest entry in the pollable/emulatable whitelist; a loaded file was
instead copying the *most-derived* id straight across. Harmless for a
protocol that maps to itself in that whitelist (Mifare Classic, tested
below), but for anything that doesn't — e.g. a Desfire dump, which resolves
through `Iso14443_4a` — Emulate's `reader_protocol_emulatable()` check would
silently fail against the wrong id. `reader_poll_protocol()` is now declared
in `reader_nfc.h` and reused from `reader_do_load()` so a loaded card
satisfies the exact same invariant a live one does.

**Known firmware-level risk, confirmed live 2026-08-02 — Emulate on a
*loaded* Mifare Classic file can hang the app, not just misbehave.**
Loaded the Mifare Classic dump above, triggered Emulate; the GUI thread
never returned. `top` kept reporting the app thread `Blocked` with no
`NfcWorker` ever appearing and a *stable* `Stack Min` (i.e. not spinning —
parked, most likely inside `nfc_listener_alloc()`/`nfc_listener_start()`),
and — notably — `loader close` reported "was closed" repeatedly while
`loader info` kept saying the app was still running: the hang is inside a
blocking SDK call the loader's normal close path cannot interrupt. Recovered
with `power reboot` over the CLI (no physical button combo needed); the
retarget/fix work above was re-verified intact afterward. This matches a
long-documented, still-recurring class of upstream issue — official
firmware [flipperdevices/flipperzero-firmware#2577](https://github.com/flipperdevices/flipperzero-firmware/issues/2577)
("Emulating of SAVED Mifare Classic not working", fixed once in 2023 and
reported recurring since) and
[DarkFlippers/unleashed-firmware#257](https://github.com/DarkFlippers/unleashed-firmware/issues/257)
("NFC stuck in emulation… must reset") — not a regression introduced by this
change: `reader_start_nfc_emulation()` is untouched, pre-existing code using
the standard `nfc_listener_alloc()`/`nfc_listener_start()` pattern, and the
*same* live-read → Emulate path is the one already verified working above.
The risk is specifically **loaded-then-emulated** data on this class of
card. Whether it is deterministic for this exact dump or intermittent (as
the upstream reports themselves describe — some tags/cards, not others) was
not cleanly isolated: a second attempt after rebuilding showed no immediate
`top`/uptime symptom, but the app was later found unresponsive to
`loader close` again before that could be confirmed either way. There is
nothing the app can do to interrupt a firmware call that gives it no
cancellation hook, so no in-app mitigation was added — but treat Load→
Emulate on Mifare Classic (and, unverified, any other protocol) as capable
of hanging the app, recoverable only via `power reboot`, not `loader close`,
until reproduced/bisected further.

**Fixed + verified 2026-08-02 — `.emv` files now carry the ISO14443-4A
transport, so a saved EMV card emulates after Load (instead of being
blocked with "no transport data").** Root cause of the original report:
`emv_save()` (emv.c) wrote only the EMV application-layer fields — the
UID/ATQA/SAK/ATS the live read had captured into `app->device` were
discarded — so `emv_load()` produced a `ReaderCardEmvFile` with no
`NfcDevice` behind it and `reader_do_emulate()` rejected it
unconditionally. Fix: `emv_save`/`emv_load` now take the `NfcDevice*` and,
in the file, append the ISO14443-4A transport with the firmware's *own*
savers (`iso14443_4a_save` writes ATQA/SAK/T0/TA(1)/TB(1)/TC(1)/T1...Tk;
UID is written explicitly, the same way `nfc_device_save` does, since
it is device-level, not protocol-level). The file version went 2 → 3;
`emv_load` still accepts v2 (financial fields only) for backward
compatibility, with `has_transport` left false so the old "no transport
data" block stays accurate for them. `reader_do_load` sets
`display_protocol`/`poll_protocol` to `NfcProtocolIso14443_4a` when
`has_transport`, and `reader_do_emulate` runs `reader_start_nfc_emulation`
for those, falling through the same path a live ISO14443-4A read uses.

Verified on device (Momentum mntm-dev, API 87.1, COM4) with a real Visa
contactless card: Save wrote `EMV_<UID>.emv` (397 B; `storage read`
showed `Version: 3` + the full EMV fields + `UID`/`ATQA: 00 44`/
`SAK: 20`/`T0: 78`/`TA(1)`/`TB(1)`/`TC(1)`/`T1...Tk` written by the
firmware's `iso14443_3a_save`/`iso14443_4a_save`). The Actions → Load →
browser flow was confirmed step by step with `top` probes:
`BrowserWorker`'s `Stack Min` rose 1092 → 1392 (the documented "the
dialog actually opened" signal — see above; note it is a one-time
low-water mark that does not recover, so it only proves the first open
in a session). The `.emv` (alphabetically first; `BrowserItemTypeBack`
is only pushed `if(!model->is_root)`, so base_path has no `..` entry) was
selected with a single `ok short`.

**The Emulate step on the loaded v3 file crashed the device** —
reproduced: `top` showed no `NfcWorker` right after the Emulate input,
then the USB VCP dropped within seconds and the device rebooted (USB
re-enumerated ~45–70 s later). This is the **same class of upstream
firmware "loaded-then-emulated" NFC listener risk already documented
above for Mifare Classic** (issue #2577/#257), now observed for
ISO14443-4A. It is *not* a defect in the new code: the fix routes the
loaded transport through the firmware's own
`nfc_device_set_data` → `nfc_listener_alloc` path — identical to what
`nfc_device_load` + `reader_start_nfc_emulation` already do for any
loaded `.nfc` card — and the live-read → Emulate path for the very same
card is the one already verified working in the session above. There is
no app-side cancellation hook for a firmware call that wedges, so no
in-app mitigation was added, consistent with the MfClassic note. Treat
Load→Emulate on EMV v3 (and, per the existing note, any other protocol)
as capable of crashing the device; recover via `power reboot` or by
waiting for the USB re-enumeration.

`reader_do_emulate`'s v2 path is safe by construction: a v2 file has no
`UID` key, so `has_transport` is never set and the
`if(!app->emv.has_transport) { notice; return; }` early return fires
*before* any `nfc_device_set_data`/`nfc_listener_alloc` call — that
early return is compile-time-provable, so the live v2 regression was not
re-forced (matching this repo's own precedent of not re-triggering the
known MfClassic hang). Backward compatibility for the v2 load itself
(financial fields only, notice on Emulate) is unchanged code from the
already-verified Load session above.

**Second fix 2026-08-02 — Load still reported "Blocked / no transport data"
for cards that withheld optional fields.** The v3 transport feature above
was correct on the *save* side, but `emv_load()` re-introduced the same
symptom through a FlipperFormat cursor bug. Root cause (read from firmware
source `lib/flipper_format/flipper_format_stream.c`): `flipper_format_read_*`
(`read_string`/`read_hex`/`read_uint32`) call `seek_to_key`, which scans
forward from the current cursor and, on a *miss*, leaves the stream at EOF
without restoring the position — unlike `get_value_count` and `key_exist`,
which both save/restore the cursor internally. The old `emv_load` read each
optional field (`Label`, `PAN`, `Expiry`, `Cardholder`, `Service Code`,
`App Preferred Name`, `Issuer Country`, `Card Sequence`, `Track2`, `Log
Count`, per-row log fields) with a bare `if(flipper_format_read_*(...))`.
The first field a card did not disclose was absent from the file, so its
read scanned to EOF and every subsequent sequential read — including `UID`
and the whole ISO14443-4A transport block — silently failed. `has_transport`
stayed false → `reader_do_emulate` showed "Blocked / no transport data". The
earlier "verified" card happened to disclose every field before any gap, so
no miss ever stranded the cursor and the bug stayed hidden. Fix:
`emv_load` now records `flipper_format_tell()` before each optional read and
`flipper_format_seek(..., FlipperFormatOffsetFromStart)` back on failure,
exactly mirroring what `get_value_count`/`key_exist` already do; the text
fields go through a small `emv_load_str` helper. Verified by a host
simulation that replays the firmware's exact `seek_to_key`/`read_value_line`
cursor semantics against (a) a full file (all fields present) — old=True,
new=True, and (b) a sparse file omitting the early optional fields —
old=False (Blocked), new=True (emulates). Build clean, APPCHK pass
(Target 7, API 87.1, Momentum mntm-dev, COM4).

**Verified 2026-08-03 — thread hygiene and stack margin under HF/LF phase
alternation.** ~5 cycles (`NFC_PHASE_MS` 1200 + `LF_PHASE_MS` 1600) sampled
across 16 `top` blocks. `top`'s AppID column attributes a spawned worker's
row to its owning app, not only the GUI thread's own row: filtering to the
row whose `stack` is 12284 (the thread `stack_size` actually controls) shows
`Stack Min` constant at 11780 throughout; `NfcScanWorker`/`NfcWorker`'s block
indices were exactly disjoint from `LfrfidWorker`'s, confirming no radio
leak across the phase transition. (One block briefly mislabelled the shared
system `TimersSrv` row under this app's AppID — a `top` in-place-redraw
artifact of the raw serial capture, not a real worker; `TimersSrv`'s own
`Stack Min` is a longstanding constant unrelated to any app.) Three
open/close cycles: `Heap` after close was 136648/54232 (free/minimum) on
cycle 1, then exactly 136624/54232 on both cycles 2 and 3 — stable, no leak.

**Verified 2026-08-03 — per-file Open/Rename/Delete/Back menu added to
Load.** Picking a file in the Load browser now opens a per-file `Submenu`
(`ReaderViewFileMenu`) instead of rendering immediately; **Open** is the
prior direct-render path, **Rename** edits the stem through a `TextInput`
(extension preserved — it picks the loader), **Delete** removes the file
and reports a real failure honestly (`storage_simply_remove()` returns
`true` when the item is already gone, so a checked `false` is the only
signal). `reader_show_notice()` gained a `back_to` parameter so a
Rename/Delete result lands on the file menu (or the actions menu after a
successful delete) instead of always on the report. `fap_version` 1.4 -> 1.5.

Build clean, APPCHK pass (Target 7, API 87.1, Momentum mntm-dev, COM4).
Confirmed on device across one uninterrupted session (uptime climbed
0h1m -> 0h56m+ with no unexplained reset): app alloc/free, the new
`ReaderViewFileMenu`/`ReaderViewRename` navigation-callback branches, the
`notice_return` mechanism, and `reader_load_abort()` (exercised via the
browser's own cancel path, both scan- and Actions-menu entry) are all
stable. **`reader_open_selected()`, the file-menu rows themselves,
`reader_do_rename_start()`/`reader_rename_result()`, and
`reader_do_delete()` were not exercised this session** — see the CLI
limitation above (Testing this app): `dialog_file_browser_show()`'s `ok`
does not register via `input send` on this firmware no matter the input
protocol used, so no file could be picked from the browser to reach them.
Isolating that limitation cost two crash-reboots and one
`dialog_file_browser_show()` hang (recovered via `power reboot`, see
above); none occurred past the browser boundary — both are inside
pre-existing, unmodified code, not this change. The two pre-existing saved
files (`Mifare_Plus_...nfc`, `EMV_...emv`) were round-tripped through a
backup directory during isolation and confirmed byte-identical back in
place (`storage list` sizes unchanged) before this session ended. Needs a
physical-button pass through steps 2-5 of this feature's own verification
plan before the new menu itself can be called device-verified.

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

Last verified **2026-08-02**, re-checked after a firmware update (`device_info`
over the CLI). Same device and SDK as Universal Card Reader above — see that
table for the fork-ABI drift detail, re-confirmed against Momentum's source at
the current commit. Re-run `device_info` before trusting this if the device
may have been reflashed again since.

| | Device | ufbt SDK |
|---|---|---|
| Target | `hardware_target` 7 | `hw_target` f7 |
| Firmware | `mntm-dev`, commit `8ed809fb`, built 03-06-2026 | official `1.4.3`, channel `release` |
| Fork | `Momentum` (Next-Flip/Momentum-Firmware) | Official |
| API | 87.1 | 87.1 |
| Port | COM3/COM4 last confirmed (Windows reassigns the port on reconnect; not a device change) — not re-checked this session | — |

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

**Verified 2026-08-03 — Auto rotation stack margin, backend disjointness,
and a static-review note closed by measurement.** 16 `top` blocks during
Auto (HF+LF) rotation with no card present: the app thread (`stack` 8188,
matching `stack_size`) held `Stack Min` constant at 7692 — clears the
4096-byte bar with room to spare, and higher than the 7424 baseline above
(this run never reached a card read, which costs more stack than an
unanswered poll). `LfrfidWorker`'s block indices were exactly disjoint from
`NfcScanWorker`/`NfcWorker`'s, confirming `rfid_stop_all()`'s stop-then-start
ordering holds under repeated rotation, not just a single manual switch.
`rfid_stop_all()` (`rfid_multi_reader.c:64-69`) does not stop `anim_timer`
before joining the outgoing backend's worker thread, unlike
`universal_card_reader`'s invariant 6 above — a static-review finding,
recorded rather than fixed because the join is bounded in milliseconds
against a 16-deep, ~1.28 s queue at `ANIM_PERIOD_MS` 80. `log debug` across
9 consecutive phase transitions measured that join cost directly: LF
transitions ran 1672–1688 ms against a 1600 ms nominal, HF transitions ran
1254–1272 ms against 1200 ms — 54–88 ms of overhead each time, confirming
the gap stays two orders of magnitude under the queue's headroom. Zero
`[W]`/`[E]` log lines across the whole window.

---

# SubGHz Auto Recorder

Third FAP in this repo, `subghz_auto_recorder/`. Listens on one user-picked
Sub-GHz frequency + modulation, auto-detects an incoming transmission by RSSI
threshold, records it to its own RAW `.sub` file, and can browse/replay
(TX)/rename/delete saved captures. A different radio and a different SDK
surface from NFC/LF, so **neither `universal_card_reader/` nor
`rfid_multi_reader/` is touched by this app.**

## Verified firmware / SDK — re-check before you build (STEP 0)

Last verified **2026-08-02** (`device_info` over the CLI). Same device and
SDK as the other two apps above. `hardware_region_provisioned` is `DE`
(Germany) — a real, non-`--`/`00` region, so the Sub-GHz region gate is
actually enforceable and testable on this device (see Testing below).

| | Device | ufbt SDK |
|---|---|---|
| Target | `hardware_target` 7 | `hw_target` f7 |
| Firmware | `mntm-dev`, commit `8ed809fb`, built 03-06-2026 | official `1.4.3`, channel `release` |
| Fork | `Momentum` (Next-Flip/Momentum-Firmware) | Official |
| API | 87.1 | 87.1 |
| Region | `hardware_region_provisioned` `DE` | — |
| Port | COM4 confirmed this session | — |

Every Sub-GHz symbol used was confirmed present in
`~/.ufbt/current/sdk_headers/f7_sdk/targets/f7/api_symbols.csv` this session,
and every struct/enum this app touches was read fresh from the SDK headers
(not from memory) before being used.

## Layout

```
subghz_auto_recorder/           <- the app; run ufbt HERE, not at repo root
  application.fam                appid subghz_auto_recorder, entry subghz_auto_recorder_app,
                                 Sub-GHz category, stack_size 12*1024
  recorder_app.h                 shared types/constants/App struct; no with_view_model calls
  subghz_auto_recorder.c         app lifetime, event router, menus, capture state machine,
                                 storage/naming, saved-signals browse/rename/delete
  recorder_radio.c / .h          radio session lifecycle, RAW capture mechanics, replay TX
  recorder_ui.c / .h             drawing + the only with_view_model call site
  icon.png / make_icon.py        10x10 1-bit icon (antenna mast + waves), regenerate with Pillow
  README.md                      app-level instructions
```

Installs to `/ext/apps/Sub-GHz/subghz_auto_recorder.fap`, i.e.
**Apps → Sub-GHz**. Captures land in `/ext/subghz/auto_rec/` — inside the
firmware's own `SUBGHZ_RAW_FOLDER`, forced by
`subghz_protocol_raw_save_to_file_init()`, so they are also visible from the
stock Sub-GHz app's Saved browser.

## Architecture

Six views on one `ViewDispatcher`: a custom animated status View
(`SubRecViewStatus` — listening/sending/notice, the only `with_view_model`
call site), two Submenus (`SubRecViewMenu` main menu,
`SubRecViewFileMenu` per-file actions), a `VariableItemList`
(`SubRecViewSettings`), a `NumberInput` (`SubRecViewNumber`, custom
frequency in kHz) and a `TextInput` (`SubRecViewText`, rename). No per-view
`view_set_previous_callback` anywhere — every Back goes through
`sub_rec_navigation_callback()`, exactly like the other two apps.

Capture is driven entirely from a 25 ms `SubRecEventRssiTick`: the timer
callback only posts (never touches the radio or the view model);
`sub_rec_handle_rssi_tick()` on the GUI thread reads RSSI, decimates the
repaint to ~8 Hz, and calls into `recorder_radio.c`'s
`sub_rec_capture_begin()`/`sub_rec_capture_end()` when the state machine
says so. A notice is an overlay flag (`app->notice_active`), never a state,
so a message raised while listening leaves the radio armed underneath it.

Five load-bearing invariants (violating any of them either crashes the
device or wedges the CC1101 driver):

1. **`FuriHalSubGhzPreset` ids 4..8 are not fork-stable.** Only ids 0..3
   (`IDLE`, `Ook270Async`, `Ook650Async`, `2FSKDev238Async`) are ever
   compiled in or passed to `subghz_devices_load_preset()`; `sub_rec_mods[]`
   only names those three, and `sub_rec_presets_self_check()` — the first
   statement of `sub_rec_app_alloc()`, before `malloc()` — asserts the
   firmware's own `subghz_block_generic_get_preset_name()` maps each label
   back to the exact `Preset:` string, so a typo fails the launch instead of
   silently corrupting every capture.
2. **`SubGhzRadioPreset` gained a `float latitude/longitude` tail on
   Momentum.** `SubRecPreset` wraps it with zeroed `float fork_tail[4]`
   headroom; every firmware call is handed `&app->preset.base`, never a bare
   `SubGhzRadioPreset` on the stack.
3. **`subghz_devices_set_frequency()` `furi_crash`es on an invalid
   frequency.** Every call site is preceded by
   `subghz_devices_is_frequency_valid()`.
4. **The CC1101 driver `furi_check`s its own state at four entry points**
   (`start_async_rx`/`_tx` need Idle, `stop_async_rx`/`_tx` need the matching
   Async state). `SubRecState` mirrors this: `sub_rec_listen_start()` and
   `sub_rec_listen_stop()` both gate on `app->state` before touching the
   radio — the `listen_stop` guard is not defensive padding, it is what
   keeps `sub_rec_app_free()` from crashing on the ordinary
   launch-then-Back-on-the-menu path, since it runs unconditionally on every
   exit.
5. **Replay's abort path and its success-teardown path are two different
   functions** (`sub_rec_tx_abort()` vs `sub_rec_tx_stop()`) because
   `stop_async_tx()` `furi_check`s that async TX is actually running — an
   abort before `start_async_tx()` succeeded must never call it.
   `sub_rec_replay()` runs every check that can fail *before* the first
   allocation and before the radio is touched, so a chained re-entry (the
   rolling-code notice) never leaks `transmitter`/`fff_tx` or double-arms
   the radio.

Rolling-code detection: `sub_rec_decoded_callback()` (SubGhzWorker thread)
sets `volatile bool app->rolling` when a decoded protocol's type is
`SubGhzProtocolTypeDynamic` (`== 2`, fork-stable). The flag is consumed at
capture end, appending `_RC` to the filename — the only persistence of the
warning; nothing reads it back out of the file.

## Testing this app

Same device/port/`cap.py` mechanics as the other two apps. Two CLI
behaviours specific to this app's testing, worth recording for next time:

- **`loader close` does not reliably force-close an app blocked inside
  `dialog_file_browser_show()`.** It reports `"...was closed"` and
  `uptime`/`loader info` still show the app running, indefinitely — the
  dialog runs its own blocking input loop and never observes the loader's
  close request. This is narrower than RFID Multi-Reader's note above
  ("`loader close` force-closes... from any UI state"): that held for every
  UI state reached through this app's *own* `ViewDispatcher`, but not for
  the separate, blocking `DialogsApp` file browser. Recovery there is a real
  `input send back short` (which the dialog's own loop *does* consume,
  returning `picked = false`), never `loader close`.
- **The file browser shows a non-file "up" entry first**, even though
  `opts.base_path` blocks navigating above it — `input send ok short`
  immediately after opening the browser does nothing observable; one
  `input send down short` first, then `ok`, actually picks the (only) file.
  Confirmed by writing a checkpoint marker to a `_trace.txt` file from each
  branch of `sub_rec_do_browse()`/`sub_rec_menu_callback()` — a `top`/
  `log debug` snapshot cannot resolve this on its own, but a file-based trace
  survives across calls without any timing pressure and is worth reaching
  for again before assuming a UI/CLI hang is a firmware bug.
- **The main menu `Submenu` remembers its cursor position across
  re-entries and wraps at the list boundary.** Two `down` presses only land
  on "Saved signals" from a *freshly launched* app (cursor starts on item 0);
  after any other visit the cursor is wherever it was left, and blind
  `down`-counting from an assumed item 0 lands on the wrong row. Always
  relaunch for a known-fresh cursor, or drive one step at a time and check
  `top` for the state that step should have caused (radio worker thread
  present/absent) before sending the next input.

**Verified 2026-08-02, on device, build clean, zero warnings, APPCHK Target
7 / API 87.1:**
- **Cold exit** (crash-rule-4 guard), both paths: Back on the main menu
  without ever entering Listen, and Listen → Back → Back. `uptime` refused
  while the app was open and answered again afterwards, strictly climbing
  across every relaunch — no reboot, no wedge.
- **Listen + the one-time ethics gate**: `top` shows exactly one
  `SubGhzWorker` thread appear after the notice auto-dismisses (no keypress),
  and disappear cleanly on Back. Confirmed across many relaunches.
- **False-positive rate**: armed 65 s with no transmitter nearby;
  `storage list /ext/subghz/auto_rec` stayed `Empty` throughout — `saved`
  never left 0.
- **Replay, full success**: selected a hand-written RAW `.sub`
  (433.92 MHz / `FuriHalSubGhzPresetOok650Async`, in-region), pressed
  Replay, and `top` caught `SubGhzFEWorker` present for the whole recorded
  duration (a 2 s test payload) before it cleanly disappeared — the entire
  parse → preset-match → transmitter alloc/deserialize → `set_tx` →
  `start_async_tx` → poll-to-completion chain ran with no crash.
- **Replay abort paths**: a file with `Preset: FuriHalSubGhzPresetCustom`
  (never a compiled-in id) was refused before `SubGhzFEWorker` ever spawned;
  a file deleted between being picked in the browser and pressing Replay hit
  the `storage_file_exists()` guard the same way. Both left `uptime`
  answering afterwards.
- **Region refusal**: a file naming 915000000 Hz — a frequency this app's
  own `sub_rec_freqs[]` table lists (so hardware-valid on the CC1101) —
  never spawned `SubGhzFEWorker` when replayed on this `DE`-provisioned
  device, consistent with the region gate (`subghz_devices_set_tx()`)
  refusing it rather than the separate frequency-validity check. [INFERENCE:
  the CLI cannot show which of the two notices fired; re-run with `log
  debug` started before a single retry to confirm the exact message if this
  matters again.]
- **Thread hygiene**: exactly one `SubGhzWorker` while listening, exactly
  one `SubGhzFEWorker` while sending, neither present at any other time,
  across dozens of launches — no duplicate-thread or leaked-thread case
  found.
- **Stack baseline (cold / Listen / replay teardown)**: `Stack Min` for the
  app thread stayed at **11300–11444 of 12284 bytes** through cold launch,
  listening, browsing, and a full replay teardown — only **840–984 bytes**
  ever used in those measured windows. This margin is far more than the 4 KB
  bar that would justify dropping to `stack_size = 8 * 1024`.
- **Stack peak (capture + replay init) — measured 2026-08-03.** `Stack Min`
  is a FreeRTOS watermark, not an instantaneous reading
  (`furi_thread_get_stack_space()`, "Get thread stack watermark" —
  `~/.ufbt/current/sdk_headers/f7_sdk/furi/core/thread.h:471–477`): it only
  falls, so one `top` sampled at the end of a session covers every code path
  that session executed, as long as the app was never closed and relaunched
  in between. Every SubGHz `Stack Min` recorded in this file before today —
  including the 11300–11444 baseline just above — was measured on a binary
  that predated this pass's reflash (installed image was 26844 bytes; the
  current tree builds 27036 bytes, `storage md5` confirmed the two match
  after reflashing); none of it is evidence about the build actually running
  today, and it is not combined with today's figure. Measured fresh, one
  continuous session, app never relaunched: Trigger set to the most
  sensitive slot (index 0, −85 dBm), ~30 s of ambient listening on the
  default 433.92 MHz fired **19 real captures** (`storage list
  /ext/subghz/auto_rec` went `Empty` → 19 `.sub` files, 0–13589 bytes), then
  **Replay** on the first of them ran to completion (no `_RC` suffix, so
  `sub_rec_replay()` skipped the rolling-code gate and transmitted
  directly). A single `top` taken after both chains had run read **`Stack
  Min` 11288 of 12284 bytes** — clears the 4096-byte pass bar by a wide
  margin. Because a real capture fired and the margin clears 4096, the flat
  "never reduce `stack_size`" instruction this bullet used to carry no
  longer applies by its own stated condition — recorded as an **option
  only, never auto-applied**: only ~1000 of 12288 bytes were used, so a
  future pass could try `8 * 1024` and re-run this same capture+replay
  protocol to confirm the margin still clears 4096 before shipping it.
- **Code review** (`code-standards` skill, mandatory pass): two findings.
  `VariableItem* s_freq_item` was a file-static mutable global — moved onto
  `SubRecApp` as `app->freq_item` (violates this file's own "no mutable
  globals" check). The notice-overlay clear (`app->notice_active` +
  `sub_rec_set_notice(..., false)`) was duplicated at two call sites —
  factored into `sub_rec_clear_notice()`. Both are local, low-risk fixes;
  rebuilt clean and re-verified Listen/ethics-gate and the Settings →
  Custom-frequency path (which reads `app->freq_item`) on device after.
  Everything else checked — thread affinity, allocation pairing, `furi_check`
  preconditions, single-writer fields, fork-ABI surface, const/scope,
  short-circuit side effects, error-path logging — passed with no changes;
  see the session's chat log for the full per-item report.

**Not verified on device (no RF transmitter was available this session):**
the RSSI-triggered auto-capture path end to end (arm → detect → record →
save), preset-name round-trip via a *live* capture in each of the three
modulations, and the continuous-carrier/cooldown behaviour (`CAPTURE_MAX_MS`
capping one file, `carrier` status, no back-to-back files). The startup
preset self-check (invariant 1 above) exercises the same
`subghz_block_generic_get_preset_name()` call the live-capture test would
observe indirectly, on real hardware, for all three modulations — strong
but not equivalent evidence. Test all three with a fixed-code transmitter
(garage/doorbell remote) before relying on the capture path in the field.

---

# Review — mandatory final step

Every task that writes or changes C ends with **two** review passes, in order:
1. **`flipper-c-review` skill** — correctness, design, Flipper failure modes.
2. **`flipper-perf-review` skill** — stack, memory, threads, redraw cost;
   static always, on-device when a device is reachable.

Invoke both by name; do not review from memory. Run after `ufbt` is
warning-clean and after any on-device verification, but before writing the
commit message. If either pass changes code, rebuild and re-verify.

**Self-run trigger:** at the end of any C-touching task these run without the
user asking. They also run on demand when the user says "review", "check
performance", "is this right", etc. A `defect` from either is fixed before the
commit message; a `smell` is fixed if local; a `note` is recorded.

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
