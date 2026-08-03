# SubGHz Auto Recorder

Flipper Zero FAP — `application.fam` description: **Sweeps the ISM bands for activity, auto-records detected signals to .sub, replays them.**

- **Version:** 1.2 (`fap_version` in `application.fam`)
- **Category:** Sub-GHz
- **Stack size:** 12 KB
- **Author:** farhadsoft

## What it does

Pick a frequency + modulation, press **Listen**, and the app arms the
internal CC1101 radio and watches RSSI. When the signal crosses your
trigger threshold it opens a RAW `.sub` file, records until the carrier
drops (or a 10 s hard cap), and returns to listening automatically —
no further input needed. Saved files are browsable from the app and can
be replayed (TX), renamed, or deleted.

All `subghz_devices_*` / `SubGhzWorker` start/stop calls happen on the GUI
thread; the worker thread and the three timers only ever post via
`view_dispatcher_send_custom_event()`.

## Usage

1. Launch the app → **Settings**: pick **Frequency** (17 common ISM
   frequencies, or scroll to **Custom** and press **OK** to type one in
   kHz), **Modulation** (AM650 / AM270 / FM238), and **Trigger** (RSSI
   threshold, -85..-60 dBm).
2. **Back** to the menu, then **Auto-record**. The first time in a session
   this shows a one-time ethics notice that dismisses itself after
   ~1.6 s; listening then starts on its own.
3. **Frequency scan** sweeps the 17 table frequencies (using the currently
   selected Modulation) and draws a 17-bar RSSI graph, ~425 ms per full
   sweep. Bars share the same -95..-35 dBm scale as the Auto-record
   screen's RSSI bar. **OK** tunes Auto-record to the peak frequency and
   starts listening immediately (also gated by the one-time ethics
   notice); **Back** returns to the menu without changing anything.
4. The screen shows the tuned frequency/modulation, a live RSSI bar with
   a tick at your trigger, and `armed` / `RECORDING` / `carrier` status.
   `carrier` means the previous capture was cut short by the 10 s cap and
   the transmitter is still keying -- recording resumes once it releases.
5. **Back** while listening stops the radio and returns to the menu.
6. **Saved signals** opens a menu: **Browse files** / **Clear all** / **Back**.
   Browse files opens the file browser scoped to `/ext/subghz/auto_rec/` --
   pick a file for **Replay** / **Rename** / **Delete**. **Clear all** asks
   `Delete N files?` with **Cancel** preselected; choosing **Delete all**
   removes every `.sub` file directly in `/ext/subghz/auto_rec/`
   (subdirectories and non-`.sub` files are left alone) and reports how many
   were deleted.
7. **Replay** re-transmits the file on its own recorded frequency and
   preset. A file whose name contains `_RC` (rolling code detected during
   capture) shows a warning first; replaying it anyway will very likely
   not open a rolling-code receiver -- see Limitations.

## Storage

Captures are written by the firmware's own RAW saver, which hardcodes
its folder to `/ext/subghz/`, so files land at
**`/ext/subghz/auto_rec/`** — inside the same tree the stock Sub-GHz
app's Saved browser already uses, and openable from there too. Filenames
are `AR_<freq/100kHz>_<HHMMSS>.sub` (e.g. `AR_4339_143022.sub` for
433.92 MHz at 14:30:22), with a `_RC` suffix appended when a rolling-code
protocol was decoded during the capture. Captures with fewer than 40 raw
edges are treated as noise and discarded automatically (the `dropped`
counter on screen).

## Limitations

- **Only AM650 / AM270 / FM238** modulations are supported. These are
  the only `FuriHalSubGhzPreset` ids (0–3) that stay at the same numeric
  value across official firmware and the Momentum fork; see
  `CLAUDE.md` for why the rest are deliberately never compiled in.
- **Rolling-code transmitters will not be defeated by Replay.** The app
  detects and flags a rolling-code protocol (via the decoder's reported
  type, not by cracking anything) and warns before transmitting, but a
  captured code is normally single-use on the real receiver.
- **Region lock can refuse TX.** If your device's provisioned region
  does not permit transmitting on a file's frequency, Replay shows
  `TX blocked` and returns cleanly; the app does not attempt to work
  around this.
- **The RSSI-threshold trigger can lose the first ~25-60 ms of a
  transmission** (one poll period plus the file-open window). Fixed-code
  remotes repeat their frame while the button is held, so a full frame is
  still captured — this is also how the stock Read RAW screen behaves.
- **The `_RC` rolling-code flag lives only in the filename suffix.**
  Renaming a file outside this app (or stripping the suffix) loses the
  warning; Rename re-appends it automatically when renaming a flagged
  file from inside the app.
- Internal CC1101 radio only — no external module (`cc1101_ext`) or OTG
  power support.
- Swept RSSI, not a spectrum analyzer. The CC1101 is single-channel; the app
  hops and samples, so a burst shorter than one sweep can be missed entirely.
  This is the same limit the stock Frequency Analyzer has.
- Bars are only comparable within one Modulation. The stock analyzer overrides
  `AGCCTRL0..2` and `MDMCFG3/4` with its own flat-response config; no
  `cc1101_*` symbol is exported to FAPs (`api_symbols.csv` contains zero
  `cc1101_` functions -- checked), so this app uses the selected preset's AGC
  instead.

## Troubleshooting

If the app exits immediately on launch with nothing on screen, check
`log error` for a `preset self-check` line — this means a
`sub_rec_mods[]` label no longer maps to the `Preset:` string the
firmware writes to `.sub` files, and the app refuses to start rather
than silently writing corrupt files.

## Building

From the app directory:

```sh
cd subghz_auto_recorder
ufbt
```

Output: `dist/subghz_auto_recorder.fap`.

To upload and run on a connected device:

```sh
ufbt launch
```

> `ufbt launch` holds the USB port. If qFlipper is open, close it first.

Alternatively, copy `dist/subghz_auto_recorder.fap` onto the SD card
under `apps/Sub-GHz/` and select **Apps → Sub-GHz → SubGHz Auto
Recorder** on the device.

The menu icon is the `icon.png` file; if needed, it can be regenerated
as a 10x10 1-bit PNG with `make_icon.py`.

## File structure

| File | Purpose |
|---|---|
| `subghz_auto_recorder.c` | App lifecycle, event router, menus, capture state machine, storage/naming, saved-signals browse/rename/delete/clear-all |
| `recorder_app.h` | Shared structs, enums, constants — no `with_view_model()` calls |
| `recorder_radio.c/h` | Radio session lifecycle, RAW capture mechanics, and Replay (TX) |
| `recorder_ui.c/h` | Device UI — the only file that calls `with_view_model()` |
| `application.fam` | FAP manifest |
| `icon.png` / `make_icon.py` | Menu icon |

## Security and ethics

Only record and replay devices you own, or that you have explicit
permission to test. Recording or replaying other people's or
organizations' RF transmissions without authorization is a legal
violation in most countries.
