# flipper-tools

A collection of RFID/NFC tools for Flipper Zero.

Currently one FAP lives in this workspace: **[universal_card_reader](universal_card_reader/)** —
a single app that reads, saves, reloads (Load), and emulates both 13.56 MHz
NFC and 125 kHz LF RFID cards.

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

Build the FAP from the app directory:

```sh
cd universal_card_reader
ufbt        # -> dist/universal_card_reader.fap
ufbt launch # build, upload, and run on the connected device
```

`ufbt launch` holds the USB port; if qFlipper is open, close it first.
On the device: **Apps → Tools → Universal Card Reader**.

For detailed build steps, supported protocols, EMV behaviour, and button
navigation, see: [universal_card_reader/README.md](universal_card_reader/README.md).
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

Only read cards you own, or that you have explicit permission to test.
Reading/recording other people's or organizations' cards without
authorization is a legal violation in most countries.
</content>
<parameter name="i">Translate root README to English