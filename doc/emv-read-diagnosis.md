# EMV read diagnosis — universal_card_reader

**Verdict: no. The app does not attempt any EMV APDU exchange, or any APDU exchange at all.**
It hands back the UID from the ISO 14443 transport layer and stops, so the PAN, expiry, AID list
and transaction log are never requested — the card is not withholding them, the app never asks.

Diagnosis date 2026-08-01. SDK `C:\Users\dev\.ufbt\current`, f7, release channel, API 87.1.
Device: Momentum `mntm-dev`, API 87.1, COM3 (`USB VID:PID=0483:5740 SER=FLIP_RCHIMANA`).

## D1 — project map

```
universal_card_reader/
  application.fam            manifest
  universal_card_reader.c    whole app, 730 lines
  icon.png / make_icon.py    10x10 1-bit menu icon
  README.md                  user docs
  dist/                      build output (universal_card_reader.fap)
```

`application.fam`, verbatim:

```python
App(
    appid="universal_card_reader",
    name="Universal Card Reader",
    apptype=FlipperAppType.EXTERNAL,
    entry_point="universal_card_reader_app",
    requires=["gui"],
    # 8 KB: the GUI thread runs the animated draw callback plus every
    # nfc_*/lfrfid_worker_* start/stop call, which is deep enough that 4 KB
    # risks a stack overflow.
    stack_size=8 * 1024,
    fap_category="Tools",
    fap_description="Reads any NFC (13.56MHz) or LF RFID (125kHz) card.",
    fap_author="farhadsoft",
    fap_version="1.1",
    fap_icon="icon.png",
)
```

App id `universal_card_reader`, `FlipperAppType.EXTERNAL`, entry point
`universal_card_reader_app` (`universal_card_reader.c:720`), `requires=["gui"]`, stack 8 KB,
installs to `/ext/apps/Tools/universal_card_reader.fap`.

NFC is reached through the high-level stack only — `universal_card_reader.c:23-32`:
`nfc/nfc.h`, `nfc_scanner.h`, `nfc_poller.h`, `nfc_device.h`, `protocols/nfc_protocol.h`, and the
five transport poller headers `iso14443_3a`, `iso14443_3b`, `iso15693_3`, `felica`, `st25tb`.
LF uses `lfrfid/lfrfid_worker.h` and `toolbox/protocols/protocol_dict.h` (lines 34-36).

## D2 — the NFC read path, and where it stops

Detection: `reader_scanner_callback` (`universal_card_reader.c:438`) receives
`NfcScannerEventTypeDetected`, picks the most-derived protocol (lines 447-453) and stores it:

- `universal_card_reader.c:455` — `app->display_protocol = best;` (this is what prints `EMV`)
- `universal_card_reader.c:456` — `app->base_protocol = protocol_base(best);`

`protocol_base()` (`universal_card_reader.c:144-151`) maps any protocol onto one of the five
**transport** protocols in `reader_base_protocols[]` (`universal_card_reader.c:135-141`:
Iso14443_3a, Iso14443_3b, Iso15693_3, Felica, St25tb) using `nfc_protocol_has_parent()`. An EMV
card is a child of ISO14443-4A, which is a child of ISO14443-3A, so `protocol_base()` returns
`NfcProtocolIso14443_3a`. **ISO14443-4A is deliberately not in that list**, so the 14443-4 layer
is skipped entirely.

Polling: `universal_card_reader.c:562` — `app->poller = nfc_poller_alloc(app->nfc,
app->base_protocol);` i.e. an ISO14443-**3a** poller for a bank card.

Stop point: `reader_poller_callback` (`universal_card_reader.c:379`). Its switch
(`universal_card_reader.c:383-406`) knows only the five transport protocols. On `Ready` it does
exactly three things:

- `universal_card_reader.c:412-413` — `nfc_poller_get_data()` then `nfc_device_set_data()`
- `universal_card_reader.c:416-419` — `nfc_device_get_uid()` into `app->scratch_id`
- `universal_card_reader.c:432-434` — post `ReaderEventNfcRead`, return `NfcCommandStop`

That is the whole read. `ReaderEventNfcRead` (`universal_card_reader.c:568-588`) copies band,
protocol name and UID into the view model; `reader_draw_callback` renders Band / Type / UID
(`universal_card_reader.c:279-303`).

**Establishing the negative:** a case-insensitive search of the source for
`apdu|send_block|iso14443_4|trx|bit_buffer|emv|ppse|0xA4` matches one line only —
`universal_card_reader.c:124`, inside the fork-ABI comment. There is no transceive call, no
`BitBuffer`, no SELECT. The app has never spoken to the application layer of any card.

The UID it does report is a random UID: bank cards rotate it per tap, which is why
`0268B2B5452000` is not a usable identifier. `README.md:114-116` already records that the test
card returns a different UID on every read.

## D3 — API surface available in the pinned SDK

The app uses the high-level `Nfc` / `NfcScanner` / `NfcPoller` abstraction and no lower-level
API. It holds **no** APDU transceive call — that is the direct, mechanical reason it stops at the
UID.

What the pinned SDK does offer, from
`sdk_headers/f7_sdk/lib/nfc/protocols/iso14443_4a/iso14443_4a_poller.h` and confirmed linkable in
`targets/f7/api_symbols.csv:2219-2224`:

| Symbol | Signature |
|---|---|
| `iso14443_4a_poller_send_block` | `Iso14443_4aError(Iso14443_4aPoller*, const BitBuffer*, BitBuffer*)` |
| `iso14443_4a_poller_send_chain_block` | same |
| `iso14443_4a_poller_send_receive_ready_block` | `(…, bool acknowledged, …)` |
| `iso14443_4a_poller_send_supervisory_block` | `(…, bool deselect, …)` |
| `iso14443_4a_poller_read_ats` | `(Iso14443_4aPoller*, Iso14443_4aAtsData*)` |
| `iso14443_4a_poller_halt` | `(Iso14443_4aPoller*)` |

`iso14443_4a_poller_send_block` is the APDU pipe. The ISO 14443-4 layer prepends the PCB and
strips it from the response (firmware 1.4.3 `iso14443_4a_poller_i.c` /
`helpers/iso14443_4_layer.c`), so the caller passes a bare APDU and gets a bare response ending in
SW1SW2. It also absorbs S(WTX) waiting-time extensions, which EMV cards use during GPO. The
header states it may only be called from inside the poller callback
(`iso14443_4a_poller.h:44`). Buffers are `BitBuffer` (`bit_buffer_alloc` /
`bit_buffer_copy_bytes` / `bit_buffer_get_data` / `bit_buffer_get_size_bytes`, all present in
`api_symbols.csv:668-697`).

To use it the app must poll `NfcProtocolIso14443_4a` instead of `NfcProtocolIso14443_3a`;
`NfcGenericEvent.instance` then carries the `Iso14443_4aPoller*`
(`iso14443_4a_poller_alloc` sets `general_event.instance = instance`).

Fork safety: `NfcProtocolIso14443_4a` is id **2** in the pinned SDK
(`sdk_headers/f7_sdk/lib/nfc/protocols/nfc_protocol.h:181`) and id **2** in Momentum `dev`, which
appends `Ntag4xx`, `Type4Tag`, `Emv` after `St25tb` and leaves ids 0–11 untouched. Momentum's
`nfc_protocol.c` registers `Emv` as a child of `Iso14443_4a`, so
`nfc_protocol_has_parent(<emv id>, NfcProtocolIso14443_4a)` is true on the device. Using id 2 does
not reintroduce a fork-sensitive sentinel.

## D4 — existing EMV / TLV helper: there is none

- `grep -i 'emv|tlv|apdu'` over `targets/f7/api_symbols.csv` → **no matches**. Nothing EMV- or
  TLV-related is linkable by a FAP on this SDK.
- No file matching `*emv*`, `*tlv*` or `*iso7816*` exists anywhere under
  `C:\Users\dev\.ufbt\current`.

Momentum's firmware does ship `nfc/protocols/emv`, but it is a fork-only protocol: the symbols are
absent from the pinned stock `api_symbols.csv`, so a FAP built here cannot link them, and building
against Momentum's SDK is out of scope. A minimal BER-TLV decoder covering only the displayed tags
must therefore be written into the app.

## D5 — does it build today, unchanged?

**Yes. No blocker zero.** `ufbt` in `universal_card_reader\` on the untouched tree:

```
scons: Entering directory `C:\Users\dev\.ufbt\current\scripts\ufbt'
        INSTALL D:\GitHub\universal_nfc_reader\universal_card_reader\dist\universal_card_reader.fap
        INSTALL D:\GitHub\universal_nfc_reader\universal_card_reader\dist\debug\universal_card_reader_d.elf
        CDB     D:\GitHub\universal_nfc_reader\universal_card_reader\.vscode\compile_commands.json
        APPCHK  C:\Users\dev\.ufbt\build\universal_card_reader.fap
                Target: 7, API: 87.1
```

No warnings, no errors, 3.1 s.

## Proposed Phase 2 change

One new module plus surgical edits to the existing read path. Nothing about the phase alternation,
the generation-stamped events or the LF worker ownership changes.

**`universal_card_reader\emv.c` / `emv.h` (new).** A single entry point,
`bool emv_read(Iso14443_4aPoller* poller, EmvData* out)`, called from inside the poller callback
because that is the only place `iso14443_4a_poller_send_block()` is legal. It contains a minimal
BER-TLV walker (definite lengths, multi-byte tags, depth-capped) that parses only the tags shown
on screen, an APDU helper that wraps `send_block` and splits off SW1SW2 — including the `61xx`
GET RESPONSE and `6Cxx` wrong-Le retries — and the command chain:

SELECT PPSE (`2PAY.SYS.DDF01`) → collect every `4F` AID and the `50` label from the `61` templates
in the FCI → SELECT the first AID that answers `9000` → read its `9F38` PDOL and build GPO data
from it with standard terminal defaults (TTQ `36 00 00 00`, amount 1.00, country/currency `0826`,
purchase, fixed unpredictable number; unknown tags zero-filled) → GET PROCESSING OPTIONS → take
the AFL from response format 1 (`80`) or 2 (`77`, harvesting `57`/`5A` directly, which is where
Visa qVSDC puts them) → READ RECORD over every AFL entry → harvest `5A` PAN, `5F24` expiry,
`5F20` name, `57`/`9F6B` track-2 as PAN/expiry fallback, `50` label. If `9F4D` was present in the
SELECT AID response, GET DATA `9F4F` for the log format and READ RECORD the log SFI, slicing each
raw record by that DOL for date `9A`, amount `9F02` and currency `5F2A`. Read commands only —
`A4`, `80 A8`, `00 B2`, `80 CA`; no WRITE or UPDATE INS byte appears in the file.

**`universal_card_reader.c`.** Four surgical changes plus a UI rework:
`reader_scanner_callback` gains `protocol_is_iso14443_4a()` and, for such cards, sets
`base_protocol = NfcProtocolIso14443_4a` so the poller runs the 14443-4 layer;
`reader_poller_callback` gains an `Iso14443_4a` case in its ready switch and calls `emv_read()`
after the UID copy; the read timeout for that path is raised to 6 s because the chain runs inside
the callback and `nfc_poller_stop()` joins the worker thread; and the Done screen becomes a
scrollable line list (Up/Down, `elements_scrollbar_pos`) so the extra fields fit on 128x64.

Every EMV field is printed only when the card returned it, otherwise as `not disclosed`. A card
that withholds the PAN over contactless — common in Europe — shows its AID plus
`PAN: not available` / `over contactless`, which is a statement about the card, not a failure of
the app.

**What no app can ever read, and this one will not pretend to:** CVV/CVC2, the PIN, and the
card's private keys never leave the secure element. The contactless profile is also a subset of
the contact profile; a card may legitimately expose only an AID.
