# flipper-tools

A collection of RFID/NFC tools for Flipper Zero.

Three FAPs live in this workspace:

- **[universal_card_reader](universal_card_reader/)** — reads, saves,
  reloads (Load), and emulates both 13.56 MHz NFC and 125 kHz LF RFID cards.
- **rfid_multi_reader** — universal RFID reader: 125 kHz LF + 13.56 MHz HF,
  read-only.
- **[subghz_auto_recorder](subghz_auto_recorder/)** — listens on one
  Sub-GHz frequency, auto-records detected signals to `.sub`, and replays
  them.
- **[ibutton_bruteforce](ibutton_bruteforce/)** — walks a curated
  iButton/1-Wire master-key table or a bounded sequential range against an
  authorized reader.

## Repo layout

```
flipper-tools/
├── universal_card_reader/   # Universal Card Reader FAP
│   ├── application.fam      # manifest
│   ├── universal_card_reader.c
│   ├── reader_nfc.c / .h    # NFC scan/poll/emulate
│   ├── reader_lf.c / .h     # LF RFID worker thread
│   ├── card_info.c / .h     # on-screen report renderer
│   ├── emv.c / .h           # contactless payment card EMV read
│   ├── reader_ui.c / .h     # device UI
│   ├── icon.png             # 10x10 menu icon
│   └── README.md            # app-level instructions
├── rfid_multi_reader/       # RFID Multi-Reader FAP
│   ├── application.fam      # manifest
│   ├── rfid_multi_reader.c
│   ├── rfid_app.h           # structs, enums, constants
│   ├── rfid_backend.h       # shared LF/HF backend interface
│   ├── backend_lf.c / .h    # 125 kHz LF backend
│   ├── backend_hf.c / .h    # 13.56 MHz HF backend
│   ├── ui.c / .h            # device UI
│   └── icon.png             # 10x10 menu icon
├── subghz_auto_recorder/    # SubGHz Auto Recorder FAP
│   ├── application.fam      # manifest
│   ├── subghz_auto_recorder.c
│   ├── recorder_app.h       # structs, enums, constants
│   ├── recorder_radio.c / .h # radio session, RAW capture, replay TX
│   ├── recorder_ui.c / .h   # device UI
│   ├── icon.png             # 10x10 menu icon
│   └── README.md            # app-level instructions
├── ibutton_bruteforce/      # iButton Brute Force FAP
│   ├── application.fam      # manifest
│   ├── ibutton_bruteforce.c # app lifetime, run engine, persistence
│   ├── brute_app.h          # structs, enums, constants
│   ├── brute_worker.c / .h  # iButton worker lifecycle + key stepping
│   ├── brute_ui.c / .h       # device UI
│   ├── master_keys.c / .h   # curated key table + self-check
│   ├── crc8_dallas.h        # Dallas CRC8
│   ├── icon.png             # 10x10 menu icon
│   └── README.md            # app-level instructions
├── doc/
│   └── emv-read-diagnosis.md  # EMV read diagnosis (historical)
├── cap.py                   # serial CLI log capture helper
├── logs/                    # cap.py output (gitignored)
├── CLAUDE.md                # development rules
└── README.md                # this file
```

## Build prerequisites

- Flipper Zero (official firmware 1.x or a fork at the same API level)
- `ufbt` — `pip install --upgrade ufbt`

## Build and run

Every app follows the same pattern — build and run from its own directory:

```sh
cd universal_card_reader   # or rfid_multi_reader, or subghz_auto_recorder
ufbt        # -> dist/<app>.fap
ufbt launch # build, upload, and run on the connected device
```

`ufbt launch` holds the USB port; if qFlipper is open, close it first.
On the device: **Apps → Tools → Universal Card Reader**,
**Apps → Tools → RFID Multi-Reader**, **Apps → Sub-GHz → SubGHz Auto
Recorder**, or **Apps → iButton → iButton Brute Force**.

For detailed build steps, supported protocols/frequencies, and button
navigation, see each app's own README:
[universal_card_reader/README.md](universal_card_reader/README.md),
[subghz_auto_recorder/README.md](subghz_auto_recorder/README.md),
[ibutton_bruteforce/README.md](ibutton_bruteforce/README.md)
(rfid_multi_reader has no separate app README yet).
For development rules (log capture, firmware fork compatibility, hardware
sequencing), see: [CLAUDE.md](CLAUDE.md).

## Log/debug workflow

Capture the device's serial CLI output with `cap.py`:

```sh
python cap.py --port COM3 --cmd "log info" --deadline 15.0
```

Transcripts are automatically written to the `logs/` directory (e.g. `logs/cap_*.log`).
For detailed CLI commands and log-safety rules, see `CLAUDE.md`.

## Ethical/legal notice

Only read, record, replay, or brute-force access systems you own, or that
you have explicit permission to test. Reading, recording, replaying, or
otherwise attempting to open other people's or organizations' cards, RF
transmissions, readers, doors, intercoms, or access systems without
authorization is a legal violation in most countries.