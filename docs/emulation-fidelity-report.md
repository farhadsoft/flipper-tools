# Emulation Fidelity Report — Universal Card Reader

This report documents what the **Universal Card Reader** FAP emulates for each
supported card family, the fidelity guarantees, and the hard limits imposed by
the Flipper Zero firmware.

## Summary

The app never substitutes or re-encodes card data before emulation. The read
path stores the exact bytes returned by the firmware poller in an `NfcDevice`
(or the LF `ProtocolDict`), and the emulate path hands that same data to the
firmware's listener (`nfc_listener_alloc()` / `lfrfid_worker_emulate_start()`).
Host-side Tier-1 tests in `test/` verify that the save-file format preserves
the fields the emulator needs.

## NFC 13.56 MHz

| Protocol | Emulated | Fidelity | Notes |
|---|---|---|---|
| ISO14443-3A | UID, ATQA, SAK | Full | Saved/loaded via `.nfc`; every byte shown is the byte emulated. |
| ISO14443-4A | UID, ATQA, SAK, ATS | Transport-layer full | Application-layer APDUs (EMV, PCDDA, etc.) are **not** relayed. The listener only answers ISO14443-4A activation; there is no applet. |
| ISO14443-3B | Not emulated | N/A | Firmware SDK 1.4.3 does **not** export `iso14443_3b_listener_alloc`, so the listener cannot be allocated. Cards are read at transport level only. |
| ISO15693-3 | UID, DSFID, AFI, IC ref, blocks read | Full | All captured blocks are replayed by the firmware listener. |
| FeliCa | IDm, PMm, system/service data | Full | Captured blocks are replayed by the firmware listener. |
| ST25TB | Not emulated | N/A | Firmware SDK 1.4.3 does **not** export `st25tb_listener_alloc`. Cards are read at transport level only. |
| Mifare Ultralight / NTAG | UID, all read pages | Full | Auth is skipped during read; all readable pages are stored and replayed. |
| Mifare Classic | UID, ATQA, SAK, all sectors with known keys | Full for readable sectors | Sectors whose keys are not known are zero-filled. New in this version: after a partial read the app attempts `MfClassicPollerModeDictAttackStandard` key recovery on the GUI thread before showing the report. Recovered sectors are then included in emulation. |
| EMV / bank card | UID, ATS | Transport-layer only | Financial data (PAN, expiry, etc.) is stored in `.emv` but never relayed during emulation. Emulation runs at ISO14443-4A transport level. |
| DESFire | Not emulated | N/A | Firmware provides no listener for DESFire emulation. |

## LF RFID 125 kHz

| Family | Emulated | Fidelity | Notes |
|---|---|---|---|
| All supported protocols (EM4100, HID Prox, Indala, etc.) | Protocol ID + raw ID bytes | Full | The firmware's `lfrfid_worker_emulate_start()` replays the exact bytes captured by `protocol_dict_get_data()`. |

## What is NOT emulated (hard limits)

- **DESFire / MIFARE Plus application-layer emulation** — requires firmware
  support that does not exist in the SDK.
- **EMV payment-card application data** — the Flipper has no secure element or
  EMV applet to answer SELECT/GPO/READ RECORD APDUs. Only UID/ATS are emulated.
- **ISO14443-3B / ST25TB** — no linkable listener allocation symbols in the
  official 1.4.3 SDK, so emulation is impossible without firmware changes.
- **Relay / MITM attacks** — the FAP SDK does not expose a listener-side APDU
  relay API; `iso14443_4a_poller_send_block()` is poller-side only.
- **iButton** — out of scope for this app (handled by the separate
  `ibutton_bruteforce` FAP).

## Known firmware issues

- Loaded-then-emulated Mifare Classic can hang the device (official firmware
  issue #2577). This is a firmware bug, not an app bug; direct emulation of a
  freshly-read card has not shown this issue.
- Loaded-then-emulated ISO14443-4A cards can hit the same class of firmware
  issue in some builds.

## Verification status

- Host Tier-1 tests pass: `test_emulation_fidelity`, `test_save_load_roundtrip`,
  and the existing `test_findmy_payload` all pass under Docker/gcc.
- `ufbt` build passes with `APPCHK` OK for official SDK 1.4.3, API 87.1.
- On-device smoke test for this change: **not performed in this session** — no
  device was connected. The Mifare Classic key-recovery path is implemented and
  compiles, but its live behavior with a partially-keyed card has not been
  verified on hardware.
