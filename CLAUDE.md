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
                               Tools category, stack_size 8*1024
  universal_card_reader.c      app/phase machinery, poller callback, views
  card_info.c / card_info.h    card report renderer + minimal NDEF parser
  emv.c / emv.h                read-only EMV (bank card) APDU chain
  icon.png / make_icon.py      10x10 1-bit icon, regenerate with Pillow
  README.md                    user-facing docs + the fork-ABI explanation
```

Installs to `/ext/apps/Tools/universal_card_reader.fap` (from `fap_category`),
i.e. **Apps → Tools** on the device.

The repo directory is named after an earlier NFC-only app that lived at the root
and was deleted once this one superseded it — that is the only reason the folder
and the app have different names. Nothing should be added back at the root.

**Git repository, branch `main`.** Commits here carry a real body: what changed,
why, and an on-device **Verified** block. Build output (`dist/`,
`.vscode/compile_commands.json`) and serial captures (`cap_*.log`) are ignored.

## Architecture

Two views + `ViewDispatcher`: the animated scan view (`ReaderViewScan`) and a
scrollable TextBox info view (`ReaderViewInfo`) that renders the report built
by `card_info_format_nfc()` / `card_info_format_lf()` into `app->info_text`
(raw pointer — the string must stay alive and unmodified while shown, so
`text_box_reset()` precedes every rebuild). Phase timing lives in the defines
at the top: `NFC_PHASE_MS` 1200, `LF_PHASE_MS` 1600, `READ_TIMEOUT_MS` 2500,
`ANIM_PERIOD_MS` 80; per-protocol read bounds come from
`reader_read_timeout_for()`.

`reader_start_nfc_phase()` / `reader_start_lf_phase()` are the only entry points
that touch a radio; both begin with `reader_stop_all()` and switch back to
`ReaderViewScan`. Flow is scan → detect → poll the most-derived safe protocol
(`reader_poll_protocol()`, e.g. MfClassic / MfUltralight / ISO14443-4A with the
EMV chain) for the full card data → render in the TextBox info screen.

**Four invariants that are load-bearing — breaking any of them wedges the device:**

1. **No fork-sensitive enum values.** `reader_poll_protocol()` picks the
   most-derived pollable protocol from a compile-time whitelist (ids 0..11),
   and `dev_has()` in card_info.c guards every `nfc_device_get_data()` call —
   both via firmware-evaluated `nfc_protocol_has_parent()`. Never reintroduce
   `NfcProtocolInvalid` or `NfcProtocolNum`; the verified device (Momentum)
   numbers them differently from the SDK we compile against.
2. **Generation-stamped events.** `EVENT_MAKE(id, gen)` / `EVENT_ID` / `EVENT_GEN`
   pack `app->gen` into the high bits; `app->gen++` on every phase change and the
   handler drops mismatches. **Keep every `ReaderCustomEvent` value below 256** —
   the upper bits are the generation. `ReaderEventAnimTick` is exempt from the check.
3. **The LF phase owns the LF worker thread.** `lfrfid_worker_start_thread()` is
   in `reader_start_lf_phase()`, not in `reader_app_alloc()`, and
   `reader_stop_lf()` calls `lfrfid_worker_stop_thread()` to *join* before NFC may
   start. `lfrfid_worker_stop()` alone does not wait.
4. **Back is owned by the ViewDispatcher's navigation callback**, never by a
   module view's `previous_callback`. `view_previous()` passes `view->context`,
   and `text_box_alloc()` sets that context to the `TextBox` itself — a
   `view_set_previous_callback()` on the info view is handed a `TextBox*` to use
   as a `ReaderApp*`. That was a real crash (see below); the dispatcher's
   navigation callback gets `event_context`, which *is* the app.
   `reader_switch_view()` is the only place that changes views: it records
   `app->current_view` (the navigation callback reads it to tell "rescan" from
   "exit") and starts/stops the animation timer, which only the scan view needs.

`protocol_dict_alloc(lfrfid_protocols, LFRFIDProtocolMax)` passes our
compile-time count against the firmware's array. Safe when the fork has more
protocols (they are simply not detected); LF ids always round-trip through the
firmware's own array, so names stay correct.

## Testing this app

Verified device: Momentum `mntm-dev`, API 87.1, **COM3**. Helper pattern:
a pyserial capture script with a hard deadline, `--cmd` for pre-capture CLI
commands and a small `--cmd-delay` so `log` attaches immediately after.

- With an NFC card on the antenna the NFC phase wins in ~300 ms every time, so
  **the LF path is only exercised with the card removed.** Confirm alternation by
  capturing ~25 s at `log debug` and checking `phase: LF (gen N)` /
  `phase: NFC (gen N+1)` alternate, then that `top` still answers.
- `input send back short` on the result screen triggers an in-app rescan (and a
  fresh read within ~300 ms). It is the only way to catch the read logs, which
  otherwise all happen before a log session can attach. **A `log` session
  accepts no further commands**, so the order is: inject the input first, then
  `log debug` — the rescan's `NFC detected` / `NFC read` lines land inside the
  capture window.
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

**Known:** the test card is an ISO14443-4A with a random UID (first byte `0x08`),
so its UID legitimately differs on every read. A real 125 kHz LF read has not yet
been verified — no LF card available.

**Fixed 2026-08-02 — Back on the result screen rebooted the device.**
`view_set_previous_callback(text_box_get_view(...), cb)` had the callback cast
`view->context` to `ReaderApp*`, but that context is the `TextBox` (8-byte
struct), so `app->phase_timer` / `app->poller` were read out of a neighbouring
heap block and passed to `furi_timer_stop()` / `nfc_poller_stop()`. Proven on
device by logging both pointers from a non-dereferencing callback:
`prev_ctx=2000A578 app=2000A5C0 text_box=2000A578`. Replaced by
`view_dispatcher_set_navigation_event_callback()` — see invariant 4.

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
