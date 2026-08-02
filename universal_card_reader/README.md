# Universal Card Reader

Flipper Zero FAP — `application.fam` description: **Reads any NFC (13.56MHz) or LF RFID (125kHz) card.**

- **Version:** 1.4 (`fap_version` in `application.fam`)
- **Category:** Tools
- **Stack size:** 12 KB
- **Author:** farhadsoft

## What it does

Scans both bands, alternating, in a single app:

- **NFC — 13.56 MHz:** ISO14443-3A/3B/4A, ISO15693-3, FeliCa, St25tb,
  Mifare Ultralight/NTAG, Mifare Classic.
- **LF RFID — 125 kHz:** protocols auto-detected via the firmware's LF RFID
  worker thread (e.g. EM4100, HID Prox, Indala, etc.).

Since Flipper cannot run both radios at the same time, the app works in
alternating phases: NFC phase (~1200 ms) → LF phase (~1600 ms) → repeat.
When a card is found, the active phase stops and the read proceeds.

All radio start/stop calls happen on the GUI thread; worker threads and
the phase timer only ever post via `view_dispatcher_send_custom_event()`.

## Displayed information

For an NFC card, on the result screen (when available):

- Band (13.56 MHz)
- Type (the most specific protocol name returned by the scanner)
- Protocol chain (e.g. `ISO14443-3A > ISO14443-4A > EMV`)
- UID
- ISO14443-3A: ATQA, SAK
- ISO14443-4A: ATS (TL/T0/TA1/TB1/TC1) and historical bytes
- ISO15693-3: manufacturer code, DSFID, AFI, IC ref, block count, and
  block contents (the ones read)
- FeliCa: IDm, PMm, blocks read/total
- Mifare Ultralight/NTAG: type, pages read/total, page hex dump,
  NDEF URI/Text records
- Mifare Classic: type (Mini/1K/4K), sectors read, sector map,
  hex dump of the read blocks
- EMV / bank card: AID(s), application label, PAN, expiry date,
  cardholder name, service code, issuer country, card sequence,
  Track2 data, transaction log (if the card provides it)

For LF RFID:

- Band (125 kHz)
- Type
- ID (hex)

EMV data is shown to whatever extent the card itself provides; a field
the card does not disclose is shown as `not disclosed` /
`not available over contactless`.

## Payment card (EMV) behaviour

For contactless bank cards (Visa, Mastercard, AmEx, Discover, JCB,
UnionPay, etc.) the app runs the following **read-only** commands at the
ISO14443-4A level:

1. SELECT PPSE (`2PAY.SYS.DDF01`)
2. SELECT AID (falls back to well-known AIDs if PPSE fails)
3. GET PROCESSING OPTIONS (with standard terminal defaults built from the PDOL)
4. READ RECORD (per the AFL)
5. GET DATA `9F4F` and READ RECORD for the transaction log

No write, update, or PIN/crypto operations are performed. CVV/CVC2, the
PIN, and the card's private keys are never extracted from the secure
element.

**Storage:** EMV cards are written to the app's own data folder
(`/ext/apps_data/universal_card_reader/EMV_<UID>.emv`) and store the PAN,
expiry date, cardholder, AIDs, Track2, and log — every financial field
that was read — plus the ISO14443-4A transport data (UID/ATQA/SAK/ATS),
so a loaded file can also be emulated just like a freshly read card.

**Emulation:** when Emulate is chosen for an EMV card, the app starts
ISO14443-4A transport-level emulation (with the captured UID/ATS,
regardless of whether the card was read live or loaded from an `.emv`
file). There is **no** application-level EMV terminal emulation; the PAN
and log are never relayed to another reader.

> **Note:** the bottom of the result screen may still show the line
> `[Policy] Bank card: emulation disabled; save stores UID/ATS only.`
> This notice is stale: in the current code, EMV data is stored in the
> `.emv` file and Emulate works at the ISO14443-4A level.

## Saving and loading cards (Save / Load)

All saved cards (`.nfc`, `.emv`, `.rfid`) are stored in a single folder —
under `/ext/apps_data/universal_card_reader/` — the app's own data
folder, separate from the previously shared `/ext/nfc` and `/ext/lfrfid`
folders. Files in those old folders are left untouched — neither moved
nor deleted.

**Load** opens from the actions menu (after Save/Emulate/Rescan) or
directly from the scan screen with the **OK** button: the firmware's own
file-picker dialog shows only this folder. The selected file is shown on
the result screen exactly like a freshly read card; files that carry
transport data (`.nfc`/`.rfid`/`.emv` with transport) can be launched
with **Emulate**. Older `.emv` files (saved before this change, containing
only EMV fields) have no transport data behind them, so they are blocked
from emulation — the result screen makes this clear.

## Limitations

- Mifare Classic sectors are only read with the transport key
  `FF FF FF FF FF FF`; sectors requiring another key will not be read.
- ISO14443-4B and SLIX emulation are not supported; these cards fall
  back to being read via the transport protocol.
- UHF / 2.45 GHz is not supported by Flipper's built-in hardware.
- EMV emulation is at the ISO14443-4A transport level only.
- **Emulating a loaded (opened-from-file) Mifare Classic card can
  sometimes hang the app** — this is a known, long-standing
  firmware-level issue (see: official firmware issue #2577, Unleashed
  issue #257), not something caused by this app's code. During the
  hang, `loader close` does not work; recovery requires restarting the
  device (`power reboot` CLI command or a physical reset). Emulating a
  freshly (live-)read card directly has not exhibited this issue.
- **Emulating a loaded (opened-from-file) EMV/ISO14443-4A card can also
  run into the same class of firmware issue** — the ISO14443-4A-confirmed
  form of the Mifare Classic note above: direct emulation of a
  freshly-read card has worked without issue many times, but the same
  card loaded from an `.emv` file and then emulated once caused a crash
  that rebooted the device (with a long USB disconnect). Recovery only
  requires `power reboot` or waiting for the device to re-enumerate; the
  app itself has no hook to intercept this firmware call.

## Building

From the app directory:

```sh
cd universal_card_reader
ufbt
```

Output: `dist/universal_card_reader.fap`.

To upload and run on a connected device:

```sh
ufbt launch
```

> `ufbt launch` holds the USB port. If qFlipper is open, close it first.

Alternatively, copy `dist/universal_card_reader.fap` onto the SD card
under `apps/Tools/` and select **Apps → Tools → Universal Card Reader**
on the device.

The menu icon is the `icon.png` file; if needed, it can be regenerated
as a 10×10 1-bit PNG with `make_icon.py`.

## Usage

1. Launch the app. The screen immediately starts scanning and shows the
   active band.
2. Hold the card against the back of the Flipper (both the NFC and LF
   antennas are there).
3. When the card is read, the result screen opens; if the content
   overflows, scroll with **Up** / **Down**.
4. On the result screen, the **Back** button opens the actions menu:
   **Save**, **Emulate**, **Rescan**, **Load**, **Exit**.
5. To open a previously saved card, select **Load** (or press **OK**
   directly from the scan screen) and pick the file from the list.
6. On the scan screen, **Back** exits the app.

## File structure

| File | Purpose |
|---|---|
| `universal_card_reader.c` | App's main lifecycle, save/emulate, phases |
| `reader_app.h` | Structs, enums, constants |
| `reader_nfc.c/h` | NFC scanner/poller, protocol resolution, emulation |
| `reader_lf.c/h` | LF RFID worker thread, emulation |
| `card_info.c/h` | Formatting of NFC/LF results for the screen |
| `emv.c/h` | Read-only EMV APDU chain, `.emv` save/load |
| `reader_ui.c/h` | Device UI (scan/reading/emulating screens) |
| `application.fam` | FAP manifest |
| `icon.png` / `make_icon.py` | Menu icon |

## Security and ethics

Only read, save, and emulate your own cards, or cards you have explicit
permission to test. Reading/recording payment card data without
authorization is a crime in most jurisdictions. Emulation is equivalent
to possessing a copy of the physical card; handle it with matching care.

## Firmware fork compatibility

The app does not use sentinel values such as `NfcProtocolNum` /
`NfcProtocolInvalid` that are not stable across forks. Protocol
relationships are evaluated by the firmware itself via
`nfc_protocol_has_parent()`. Verified against official firmware 1.x and
Momentum `mntm-dev` (API 87.1).

APPCHK only compares API major/minor; forks can keep the same API
version while shifting enum values. To guard against this, the app only
uses protocol IDs (0–11) that remain identical between official and
Momentum firmware for polling and emulation.
</content>
<parameter name="i">Translate universal_card_reader README to English