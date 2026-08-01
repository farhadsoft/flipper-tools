# universal_nfc_reader

Holds one Flipper Zero app: **[universal_card_reader](universal_card_reader/)** —
reads both NFC 13.56 MHz and LF RFID 125 kHz cards in a single FAP by alternating
timed phases. Contactless EMV bank cards get a full application-layer read (AID,
PAN, expiry, cardholder name, transaction log), not just a UID.

Build and flash from that directory, not from here:

```sh
cd universal_card_reader
ufbt          # -> dist/universal_card_reader.fap
ufbt launch   # build, upload and run on a connected Flipper (USB)
```

Then on the device: **Apps → Tools → Universal Card Reader**.

See [universal_card_reader/README.md](universal_card_reader/README.md) for usage,
supported protocols and the firmware-fork compatibility notes, and `CLAUDE.md`
for development guidance.

An earlier NFC-only app (`universal_nfc_reader.c`) lived at this level and was
removed once the dual-band app superseded it — hence the directory name.

Use only on cards and tags you own or are authorised to test.
