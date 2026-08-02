# Universal Card Reader (Flipper Zero FAP)

Reads **both** card families with one app:

- **NFC** — 13.56 MHz: ISO14443-3A/3B, ISO15693-3, FeliCa and everything built on top
  of them (Mifare Classic, Ultralight, DESFire, …). Contactless **EMV bank cards**
  additionally get an application-layer read — AID(s), application label, PAN,
  expiry, cardholder name and transaction log — not just a UID.
- **LF RFID** — 125 kHz: EM4100, HID Prox, Indala, and the rest of the firmware's
  LF protocol set

## How it works

The Flipper cannot drive both radios at once, so the app alternates timed phases and
loops until something is found:

```
NFC phase (~1200 ms)  ->  LF phase (~1600 ms)  ->  NFC phase  ->  ...
```

During the NFC phase the firmware's `NfcScanner` looks for a card; when one appears the
phase timer is cancelled (so a read is never cut off mid-transaction) and the card's
base transport protocol is polled for its UID. During the LF phase the LF RFID worker
runs an auto-detect read (ASK and PSK).

All radio start/stop calls happen on the GUI thread; the worker threads and the phase
timer only post events to it. That keeps the NFC and LF stacks from ever being active
at the same time.

Cards that speak ISO14443-4 (bank cards, and anything else layered on the same
transport) are polled one level deeper: after the UID, the app runs a read-only
EMV command chain entirely inside the poller callback, described below. Every
other card keeps reading only its UID.

## EMV bank cards

Contactless bank cards (Visa, Mastercard, …) are read past the UID with a
hand-rolled, read-only EMV command chain — the only commands issued anywhere are
SELECT, GET PROCESSING OPTIONS, READ RECORD and GET DATA; there is no write or
update:

1. **SELECT PPSE** (`2PAY.SYS.DDF01`) to list the card's payment application(s).
2. **SELECT AID** on the first application that answers.
3. **GET PROCESSING OPTIONS**, built from the application's PDOL with standard
   terminal defaults (a nominal purchase, fixed country/currency, …). These
   values affect risk management on the card's side, not what it discloses.
4. **READ RECORD** across every entry in the returned Application File Locator.
5. If the card advertises a transaction log (tag `9F4D`), **GET DATA** for its
   log format and **READ RECORD** each log entry.

The result screen shows whatever the card actually returned — AID(s),
application label, PAN, expiry, cardholder name, transaction log — nothing
more. PAN and expiry are shown in full: it's your own card, and masking data
the card already handed the terminal buys no real protection. A field the card
withholds reads as **"not disclosed"** (or **"not available" / "over
contactless"** for a PAN withheld from the contactless interface specifically,
which is common on European cards) instead of being silently dropped, so it's
clear the app asked and the card declined.

**What no reader can ever get, this one included:** the CVV/CVC2, the PIN, and
the card's private keys never leave its secure element. A contactless read is
also a subset of what a contact (chip-and-PIN) terminal sees — a card may
legitimately expose only its AID over NFC.

## Requirements

- Flipper Zero on **official firmware 1.x** or a fork at the same API level
  (verified on Momentum `mntm-dev`, API 87.1)
- [`ufbt`](https://github.com/flipperdevices/flipperzero-ufbt) — `pip install --upgrade ufbt`

### Firmware forks: a matching API version is not enough

`APPCHK` only compares the API major/minor, and a fork can keep the same version
while changing enum values. Momentum, for example, adds `Ntag4xx`, `Type4Tag` and
`Emv` to `NfcProtocol`, which moves `NfcProtocolInvalid` from 13 to 16, and adds two
entries to `LFRFIDProtocol` (one of them *inserted mid-enum*, shifting later ids).

A FAP that compiles a fork-sensitive sentinel into itself and then compares it
against an id the firmware returned will silently take the wrong branch, and can
hand an out-of-range protocol to `nfc_poller_alloc()` — which `furi_check`s it and
crashes the device. This app therefore never uses `NfcProtocolInvalid` or
`NfcProtocolNum`; it resolves a card's transport protocol with
`nfc_protocol_has_parent()`, evaluated by the running firmware, against the handful
of transport ids that are identical across forks. Keep it that way.

The one addition to that list, `NfcProtocolIso14443_4a` (needed to reach the
EMV application layer), is safe for the same reason: it is id 2 in both this
SDK and Momentum `dev`, sitting well inside the range (0–11) both forks leave
untouched — the divergence only starts at the ids Momentum appends afterward.

## Build

From this directory:

```sh
ufbt
```

The app lands at `dist/universal_card_reader.fap`. Built and verified against SDK
**API 87.1, target 7**.

To build and run it on a connected Flipper in one step:

```sh
ufbt launch
```

Or copy `dist/universal_card_reader.fap` to the SD card under `apps/Tools/` with qFlipper
and start it from **Apps → Tools** on the device.

## Usage

1. Launch the app. It immediately starts cycling between the two bands and shows which
   one it is currently scanning.
2. Hold a card against the back of the Flipper — the NFC antenna and the 125 kHz coil
   are both there.
3. When a card is read, the result screen lists everything found, one item per
   line — scroll with **Up**/**Down** if there are more lines than fit:
   - **Band** — `13.56 MHz` or `125 kHz`
   - **Type** — the detected protocol name
   - **UID**/**ID** — in hex
   - For EMV cards: **AID**, **App** (label), **PAN**, **Expiry**, **Name** and
     **Txn log** entries — see [EMV bank cards](#emv-bank-cards) — or "No EMV
     app on card" if it has no payment application at all
4. **Back** on the result screen goes straight back to scanning for the next
   card; **Back** on the scanning screen exits the app. If a read fails, the
   "Read failed" screen offers **OK** to retry and **Back** to exit.

If a card is not picked up right away, keep it in place for a couple of seconds — it may
need to wait through one full phase cycle before its band's turn comes around. Some
cards also read more reliably at a slightly different position on the back panel.

## Legal / responsible use

This tool is for cards and tags that **you own, or that you have explicit permission to
test**. Reading access-control credentials — or payment card data — belonging to other
people or to an organisation without written authorisation is illegal in most
jurisdictions. Use it for your own tags and cards, for authorised security assessments,
and for learning — nothing else.

## History

An earlier NFC-only app lived at the repository root and gave the directory its
name; it was deleted once this dual-band app superseded it. It is worth knowing
why it failed, because it is the same defect described under "Firmware forks"
above wearing a different mask: with an EMV card it hung on "Reading" for ever
rather than crashing. Momentum's `NfcProtocolType4Tag` is 13, exactly the value
this SDK uses for `NfcProtocolInvalid`, so a loop walking up to the sentinel
stopped one level early and returned EMV as if it were a transport protocol. The
poller allocated for it passed `furi_check` — 13 is a legal id on that firmware —
but never reported `Ready`, and nothing timed the read out.

Hence the two defences this app keeps: no compiled-in sentinels, and a bounded
read (`READ_TIMEOUT_MS`) so a read that cannot complete always surfaces as an
error instead of a hang.
