# SubGHz Auto Recorder

Flipper Zero FAP — `application.fam` description: **Sweeps the ISM bands for activity, auto-records and decodes detected signals to .sub, replays them.**

- **Version:** 1.4 (the FAP manifest is `universal_toolkit/application.fam`;
  this module ships inside the Universal Toolkit, not as its own FAP)
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

When a burst also decodes as a known protocol, the app writes a second
file (`<stem>_D.sub`, never in place of the RAW capture) and shows the
protocol name **and its key hex** on the Listening screen while the burst
is still on air — the stock Read screen's live readout, without having to
save first.

Beyond record-and-replay, the module covers the rest of the Flipper's
Sub-GHz surface that a FAP can reach: **Add manually** builds a `.sub` for
any encodable protocol from a bit count and a key you type (the protocol
list is read from the firmware's own registry, so it cannot go stale);
**Hopping** sweeps the firmware's hopper frequency list while armed and
dwells where a signal is; **Sound** mirrors the received data to the
speaker; **Alert** plays a beep/vibro/LED on every saved capture and sent
file; **Ignore** drops captures that decode as a protocol you named (OK on
the Listening screen ignores whatever is on it); **Browse SD card** opens
the whole `/ext/subghz/` tree, so signals saved by the stock Sub-GHz app
replay and analyze here too; and **Keystore** loads the KeeLoq
manufacture-key database that the secure families need (opt-in — see
Limitations).

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
   While a burst decodes, two lines replace the status word: the
   firmware's own `<name> <bits>bit` label and `Key <hex>`.
   `carrier` means the previous capture was cut short by the 10 s cap and
   the transmitter is still keying -- recording resumes once it releases.
   **OK** adds the protocol currently on screen to the Ignore list; its
   captures are then dropped (counted as dropped, logged as `ignored`).
5. **Back** while listening stops the radio and returns to the menu.
6. **Saved signals** opens a menu: **Browse captures** / **Browse SD
   card** / **Stats** / **Clear all** / **Delete RAW** / **Delete
   decoded** / **Delete _RC** / **Back**. Browse captures opens the file
   browser scoped to `/ext/subghz/auto_rec/`; Browse SD card opens the
   whole `/ext/subghz/` tree, which is where the stock Sub-GHz app's own
   saves live -- pick either kind for **Replay** / **Analyze** / **Label**
   / **Rename** / **Delete**. The batch deletes stay scoped to
   `/ext/subghz/auto_rec/` on purpose and never reach the stock app's
   files. **Clear all** asks `Delete N files?` with **Cancel**
   preselected; choosing **Delete all** removes every `.sub` file
   directly in `/ext/subghz/auto_rec/` (subdirectories and non-`.sub`
   files are left alone) and reports how many were deleted.
7. **Replay** re-transmits the file on its own recorded frequency, preset,
   and protocol -- a decoded file (`_D.sub`) replays as that protocol, a
   RAW file replays as RAW. A file whose name contains `_RC` (rolling code
   detected during capture) shows a warning first; replaying it anyway will
   very likely not open a rolling-code receiver -- see Limitations.
8. **Analyze** is read-only: no radio call, no file write. It shows an
   **Info** page (frequency, modulation, protocol, sample count, file size,
   and a `Bit`/`Key` line for any file that carries them -- decoded by this
   app or not) and a **Waveform** page (the burst's pulse train, downsampled
   to 120 columns; empty for a decoded file, which has no `RAW_Data`).
   **Left**/**Right** toggles between the two pages; **Back** returns to the
   file menu.
9. **Add manually** lists every protocol the firmware can encode (read
   from its registry at menu-open time), then asks for a bit count
   (1-64, default 24) and the key on a byte keyboard. The file is built
   by the firmware's own serializer with your Bit/Key written over it,
   validated by a trial deserialize, and saved into
   `/ext/subghz/auto_rec/` -- the notice lands you on the new file's menu
   with **Replay** one OK away.
10. **Settings** also carries **Hopping**, **Sound**, **Alert** and
    **Keystore** (all Off by default) and **Ignore** (the current list;
    OK on a row un-ignores it, **Clear all** empties it). Hopping, Sound
    and Keystore take effect at the next Listen, not mid-session.

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

When a burst also decodes as a known protocol, a second file,
`<same stem>_D.sub`, is written next to the RAW capture -- never in place
of it, so a false decode on noise never costs you the raw recording.

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
- **The Analyze waveform is midpoint-sampled to 120 columns**, so a pulse
  narrower than `total duration / 120` can vanish or double. It shows the
  burst's shape, not measurement-grade edges.
- **Keystore is opt-in (Settings > Keystore, default Off).** The parsed
  KeeLoq manufacture-key database is resident for the whole module
  session and runs to several kilobytes, while this module's heap
  minimum was measured at 2448 bytes free without it (Momentum
  `mntm-dev`, 2026-10-02). With the row Off, KeeLoq-family protocols
  (Came Atomo, Nice Flor-S, Alutech AT-4N, manufacture-keyed
  DoorHan/LiftMaster) neither decode nor replay -- exactly the pre-1.4
  behaviour. Turn it On only after re-measuring the heap margin on your
  device.
- **Hopping tears the receiver down between frequencies.** Each hop
  stops async RX, retunes, and restarts it (stock does the same), so a
  burst that lands inside a hop's ~ms window is missed, and the RSSI
  reading that triggered a capture is always from the frequency the
  radio is now on.
- **Add manually trusts your bit count.** The app validates the result
  by deserializing it (a protocol that rejects the count refuses to
  save), but it cannot tell you the right count for a given receiver;
  protocols with a timing field get `TE: 400` us, the stock default.

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
| `recorder_parse.c/h` | HAL-free parser for a decoder `get_string()` frame (the live readout); pinned by `test/test_recorder_parse.c` |
| `recorder_ui.c/h` | Device UI — the only file that calls `with_view_model()` |
| (manifest) | `universal_toolkit/application.fam` -- this module is hosted by the toolkit and has no `.fam` of its own |
| `icon.png` / `make_icon.py` | Menu icon |

## Security and ethics

Only record and replay devices you own, or that you have explicit
permission to test. Recording or replaying other people's or
organizations' RF transmissions without authorization is a legal
violation in most countries.
