# flipper-tools

A set of [Flipper Zero](https://flipperzero.one/) external apps (FAPs) written in C, plus
**Universal Toolkit** — a single launcher app that hosts them as modules behind one menu, one shared
log, and one lifecycle contract. Built and tested on real hardware (Momentum `mntm-dev`, API 87.1,
target 7) with `ufbt`.

This README is written to be honest about **both** what the tools do and where they hit hard limits
that no code can cross — because half of using RF/NFC hardware well is knowing what is actually
possible.

---

## Contents

| App / module        | Radio / interface        | What it does                                              |
|---------------------|--------------------------|----------------------------------------------------------|
| **subghz_auto_recorder** | Sub-GHz (CC1101)    | Auto-record by RSSI, frequency scan, analyze, decode, replay |
| **universal_card_reader** | 13.56 MHz NFC + 125 kHz LF | Read/parse cards (MIFARE, NTAG, read-only EMV, EM4100/HID/Indala), emulate saved cards |
| **rfid_multi_reader** | 125 kHz LF RFID        | Read / emulate / write LF tags                            |
| **universal_toolkit** | launcher + all of the above as modules | One menu, per-module lifecycle, unified session log; adds **gpio_info** and **ble_findmy** |
| &#x21B3; **gpio_info**     | GPIO                     | Header pin state monitor (also the lifecycle proof module) |
| &#x21B3; **ble_findmy**    | BLE (STM32WB core2)      | Broadcast an OpenHaystack Find My beacon (see limits below) |

The three standalone apps remain individually buildable; the toolkit wraps them as modules.

---

## Features

**subghz_auto_recorder** — the most developed app. Arms and auto-saves Sub-GHz captures above an RSSI
threshold; swept-RSSI frequency scanner with peak lock; per-capture analyze (info + downsampled RAW
waveform with zoom/pan) and decode of fixed-code protocols; generalized replay (RAW or decoded); file
labels, batch delete, a stats screen, saved frequency/profile config, capture schedule/limit, and
session-scoped decoded-duplicate detection.

**universal_card_reader** — reads and parses 13.56 MHz (MIFARE Classic/Ultralight, NTAG, read-only
EMV) and 125 kHz LF (EM4100, HID, Indala), with saved-file management and emulation of your own saved
cards.

**ble_findmy** — turns the Flipper into an OpenHaystack "Find My" beacon for locating **your own**
device via Apple's network. Broadcasts one static SECP224R1 public key; crypto/keygen happen
off-device. **Read the limits section before expecting it to behave like an AirTag.**

---

## Honest capabilities & limits

These are properties of the systems involved, not missing features:

- **The Find My beacon never appears in the iPhone "Find My" app.** Only genuine AirTags and Apple
  **MFi-certified** accessories (with Apple's proprietary chip) register there. An OpenHaystack DIY
  beacon is *tracked* by the network but its location is retrieved through **your own desktop tool**
  (OpenHaystack / macless-haystack + an Apple ID + an anisette server), never inside the Find My app.
  The desktop retrieval side is **not included** in this repo yet — the FAP is only the beacon half.
- **Rolling-code Sub-GHz cannot be replayed.** Decode can identify a rolling-code remote; a replay will
  not open the receiver (counter desync). This is by design.
- **Crypto-secured cards cannot be cloned/emulated identically.** DESFire, EMV bank cards, and secure
  access badges keep their keys in a non-extractable secure element. LF tags and fully-read MIFARE
  Classic (all sectors/keys) *can* be emulated faithfully; incomplete reads cannot.
- **"Spectrum" is swept RSSI, not an SDR waterfall** — the CC1101 is single-channel.

---

## Install & build

Requires [`ufbt`](https://github.com/flipperdevices/flipperzero-ufbt) and a Flipper on the matching
firmware (developed against Momentum `mntm-dev`, API 87.1). Build from an **app directory**, not the
repo root:

```sh
cd subghz_auto_recorder && ufbt launch      # build, install, and run one app
cd universal_toolkit    && ufbt launch      # the launcher + all modules
```

> The repo root holds several runnable apps, so `ufbt` from the root reports *"More than one app is
> runnable"* — always `cd` into the specific app first. For the toolkit,
> `ufbt launch APPID=universal_toolkit` disambiguates if needed. (A cleaner `apps/` + shared-`lib/`
> layout is planned — see Known issues.)

---

## Repository structure

```
flipper-tools/
  README.md                    this file
  CLAUDE.md                    conventions + invariants for AI coding agents
  docs/                        design docs, plans, reviews
  cap.py / stress_cr_exit.py / uart_capture.py / repro_menu_exit.py   test/repro toolchain
  test/                        host (Tier-1) unit tests — plain gcc, `make test`
  universal_toolkit/           launcher app (core + modules/<name>/…)
  universal_card_reader/  subghz_auto_recorder/  rfid_multi_reader/   standalone apps
```

---

## Architecture (Universal Toolkit)

Clean-Architecture-style layering: **domain** (pure C, no Flipper deps — payload/parse/math, host-
testable) → **module** (lifecycle, views, events) → **HAL** (`furi_hal_*`, GUI, storage). The
`ToolkitModule` descriptor is the plug-in contract; the launcher dispatches through it.

**Module lifecycle contract (the load-bearing rule):** one module active at a time; on enter it
registers its own view-id block, allocates, and acquires its peripheral; on exit it switches to the
launcher **first**, then releases the peripheral, frees, and removes its views, and bumps a generation
counter so no stale event/tick is processed. Module exit is always **deferred off the input-delivery
stack** (a reserved `0xFF` self-posted event) — never run synchronously inside a nav/menu callback.
Full detail in `CLAUDE.md`.

---

## Testing

Two tiers, by necessity:

- **Tier 1 — host unit tests** for HAL-free domain code (`test/`, plain `gcc`, `make test`). Fast
  red-green-refactor; new domain code is written test-first. Example: the Find My advertisement byte
  layout is locked by known-answer vectors, so a wrong beacon payload fails on the host in
  milliseconds — no device needed.
- **Tier 2 — on-device acceptance** for anything touching a radio/GUI/USB, driven over the CLI
  (`cap.py` and friends): `storage`/`loader info`/`uptime`/`top` as the ground-truth signals. Some
  paths (file-browser picks, BadUSB, actual RF on-air) are physical-only and are recorded as such.

The repro/verification scripts (`stress_cr_exit.py`, `repro_menu_exit.py`, `uart_capture.py`) double
as regression checks for specific crash classes.

---

## Status & known issues

- **Module menu-Exit crash — fixed & confirmed** (deterministic, with a pre-fix negative control and a
  physical re-run). See `CLAUDE.md` "Module-exit crash fixes."
- **Back/nav idle-exit crash — fixed, rarer but not proven.** Intermittent; the exact `file:line` was
  never captured (UART capture staged for next time). Clean over 410 CLI + 48 physical cycles.
- **`loader close` during an active NFC/LF scan — open, reported not reproduced-with-capture.** Idle
  loader-close is clean; the active-scan crash needs a captured repro before any fix.
- **Repo layout debt — open.** The wrapped apps still live inside `universal_toolkit/`; the planned
  `apps/` + shared-`lib/` extraction (so the toolkit links reusable cores instead of duplicating) is
  not done, which is why `ufbt` from the root is ambiguous.
- **ble_findmy retrieval side — not built.** The beacon broadcasts; seeing its location needs the
  desktop retrieval workflow, which is not in this repo yet.

Verification claims in `CLAUDE.md` and `docs/` are kept at their true level — *confirmed* vs
*rarer/not proven* vs *reported/not reproduced* — and never rounded up.

---

## Scope & responsible use

Everything here is scoped to **the user's own equipment**: capturing/replaying your own remotes,
reading/emulating your own cards, locating your own device, and a personal logging/review tool. That
is the same class as public capture-and-replay research and is fully supported.

Deliberately **out of scope**, and not implemented: optimized brute-force / key-space (De Bruijn)
attacks against systems you don't own; adapting attack code aimed at unknown receivers; BLE
advertisement spam/DoS of nearby devices; and rotating Find My identities to defeat Apple's
unknown-tracker (anti-stalking) alert. Conceptual explanations are fine; uplift for unauthorized
access is not.

Use only on devices and systems you own or are authorized to test. You are responsible for complying
with local law.

---

## Docs

`docs/` holds the design and decision records: the toolkit architecture & capability map, the project
structure & conventions, per-feature execution plans and their reviews, and the Find My advertisement
spec. `CLAUDE.md` holds the invariants any contributor (human or AI agent) must follow, including the
module-exit crash fix record.

## Credits & references

Sub-GHz capture/replay builds on the standard Flipper CC1101 workflow. The Find My beacon follows the
[OpenHaystack](https://github.com/seemoo-lab/openhaystack) format (SEEMOO Lab) and is the same approach
as [FindMyFlipper](https://github.com/MatthewKuKanich/FindMyFlipper). Built with Flipper's `ufbt`.

## License

_Add a license file (e.g. MIT) before publishing — until then, all rights reserved by the author._
