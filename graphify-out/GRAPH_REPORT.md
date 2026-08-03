# Graph Report - .  (2026-08-03)

## Corpus Check
- Corpus is ~40,833 words - fits in a single context window. You may not need a graph.

## Summary
- 433 nodes · 946 edges · 35 communities (31 shown, 4 thin omitted)
- Extraction: 86% EXTRACTED · 14% INFERRED · 1% AMBIGUOUS · INFERRED: 132 edges (avg confidence: 0.8)
- Token cost: 0 input · 0 output

## Community Hubs (Navigation)
- [[_COMMUNITY_Sub-GHz Recorder App State & Radio|Sub-GHz Recorder App State & Radio]]
- [[_COMMUNITY_Universal Card Reader App State & LFNFC Workers|Universal Card Reader App State & LF/NFC Workers]]
- [[_COMMUNITY_EMV TLV Parsing & Data Harvesting|EMV TLV Parsing & Data Harvesting]]
- [[_COMMUNITY_RFID Multi-Reader HF Backend|RFID Multi-Reader HF Backend]]
- [[_COMMUNITY_Repo-Level Dev Rules & Documentation|Repo-Level Dev Rules & Documentation]]
- [[_COMMUNITY_EMV APDU Read & Selection Flow|EMV APDU Read & Selection Flow]]
- [[_COMMUNITY_Canvas Draw Helpers (Sub-GHz + Card Reader UI)|Canvas Draw Helpers (Sub-GHz + Card Reader UI)]]
- [[_COMMUNITY_Card Info Formatting & Display|Card Info Formatting & Display]]
- [[_COMMUNITY_EMV AID Database LoadSave|EMV AID Database Load/Save]]
- [[_COMMUNITY_RFID Multi-Reader UI Draw Helpers|RFID Multi-Reader UI Draw Helpers]]
- [[_COMMUNITY_Universal Card Reader Packaging & Icon|Universal Card Reader Packaging & Icon]]
- [[_COMMUNITY_Sub-GHz Recorder Packaging & Icon|Sub-GHz Recorder Packaging & Icon]]
- [[_COMMUNITY_Community 14|Community 14]]
- [[_COMMUNITY_Community 33|Community 33]]
- [[_COMMUNITY_Community 34|Community 34]]

## God Nodes (most connected - your core abstractions)
1. `RfidBackend Vtable Struct` - 23 edges
2. `out_addf()` - 19 edges
3. `reader_stop_all()` - 18 edges
4. `ReaderApp struct` - 15 edges
5. `sub_rec_app_alloc()` - 14 edges
6. `reader_custom_event_callback()` - 14 edges
7. `hf_describe()` - 13 edges
8. `rfid_custom_event_callback()` - 13 edges
9. `reader_switch_view()` - 13 edges
10. `reader_do_load()` - 13 edges

## Surprising Connections (you probably didn't know these)
- `Proposed EMV APDU Read Chain (PPSE->AID->GPO->READ RECORD)` --rationale_for--> `emv_read()`  [EXTRACTED]
  doc/emv-read-diagnosis.md → universal_card_reader/emv.c
- `GUI Thread Owns the Radio / View Model` --conceptually_related_to--> `reader_stop_all()`  [INFERRED]
  CLAUDE.md → universal_card_reader/universal_card_reader.c
- `Fork ABI Drift (enum/struct layout not API-version-gated)` --references--> `dev_has()`  [EXTRACTED]
  CLAUDE.md → universal_card_reader/card_info.c
- `EMV Read Diagnosis (universal_card_reader)` --rationale_for--> `emv_read()`  [EXTRACTED]
  doc/emv-read-diagnosis.md → universal_card_reader/emv.c
- `Fork ABI Drift (enum/struct layout not API-version-gated)` --references--> `reader_poll_protocol()`  [EXTRACTED]
  CLAUDE.md → universal_card_reader/reader_nfc.c

## Hyperedges (group relationships)
- **EMV APDU Command Chain (PPSE -> AID -> GPO -> Records -> Log)** — universal_card_reader_emv_c_emv_read, universal_card_reader_emv_c_emv_select_ppse, universal_card_reader_emv_c_emv_try_fallback_aids, universal_card_reader_emv_c_emv_select_aid, universal_card_reader_emv_c_emv_gpo, universal_card_reader_emv_c_emv_read_records, universal_card_reader_emv_c_emv_read_log [EXTRACTED 0.95]
- **EMV .emv FlipperFormat save/load field set** — universal_card_reader_emv_c_emv_save, universal_card_reader_emv_c_emv_load, universal_card_reader_emv_h_emvdata [EXTRACTED 0.90]
- **EMV read -> report render pipeline (poller callback into card_info)** — universal_card_reader_reader_nfc_c_reader_poller_callback, universal_card_reader_emv_c_emv_read, universal_card_reader_card_info_c_card_info_format_nfc [INFERRED 0.85]
- **Every radio action routed through the GUI-thread dispatcher** — universal_card_reader_universal_card_reader_reader_custom_event_callback, universal_card_reader_universal_card_reader_reader_do_save, universal_card_reader_universal_card_reader_reader_do_emulate, universal_card_reader_universal_card_reader_reader_do_load [INFERRED 0.85]
- **LF radio stop -> thread join -> restart handover** — universal_card_reader_universal_card_reader_reader_stop_all, universal_card_reader_reader_lf_reader_stop_lf, universal_card_reader_reader_lf_reader_start_lf_phase [INFERRED 0.80]
- **Generation counter bump/check lifecycle for stale event filtering** — universal_card_reader_reader_app_h_readerapp, universal_card_reader_universal_card_reader_reader_custom_event_callback, universal_card_reader_reader_lf_reader_lf_callback [INFERRED 0.80]
- **Stop-Free-Start Radio Band Handover** — rfid_multi_reader_rfid_multi_reader_rfid_stop_all, rfid_multi_reader_rfid_multi_reader_rfid_start_scan, rfid_multi_reader_rfid_multi_reader_rfid_advance_phase, rfid_multi_reader_backend_hf_hf_scan_stop, rfid_multi_reader_backend_lf_lf_scan_stop [INFERRED 0.85]
- **Generation-Stamped Stale Event Guard** — rfid_multi_reader_rfid_multi_reader_rfid_on_detect, rfid_multi_reader_rfid_multi_reader_rfid_on_read, rfid_multi_reader_rfid_multi_reader_rfid_custom_event_callback, rfid_multi_reader_rfid_app_rfidapp [EXTRACTED 1.00]
- **Backend Alloc/Release Across App Lifetime** — rfid_multi_reader_rfid_multi_reader_rfid_app_alloc, rfid_multi_reader_rfid_multi_reader_rfid_app_free, rfid_multi_reader_backend_hf_hf_alloc, rfid_multi_reader_backend_hf_hf_release, rfid_multi_reader_backend_lf_lf_alloc, rfid_multi_reader_backend_lf_lf_release [EXTRACTED 1.00]
- **Capture open -> record -> save/discard lifecycle** — subghz_auto_recorder_subghz_auto_recorder_sub_rec_handle_rssi_tick, subghz_auto_recorder_recorder_radio_sub_rec_capture_begin, subghz_auto_recorder_recorder_radio_sub_rec_capture_end, subghz_auto_recorder_recorder_radio_sub_rec_capture_finish [EXTRACTED 0.90]
- **Listen stop -> radio teardown handover (stop before free/start-next-radio)** — subghz_auto_recorder_recorder_radio_sub_rec_listen_stop, subghz_auto_recorder_recorder_radio_sub_rec_capture_finish, subghz_auto_recorder_recorder_radio_sub_rec_radio_free [INFERRED 0.75]
- **Replay TX start -> poll -> stop/abort lifecycle** — subghz_auto_recorder_recorder_radio_sub_rec_replay, subghz_auto_recorder_recorder_radio_sub_rec_handle_tx_poll, subghz_auto_recorder_recorder_radio_sub_rec_tx_stop [EXTRACTED 0.90]
- **Strict Stop-Then-Start Radio Handover Across Apps** — claude_md_colliding_radios, universal_card_reader_universal_card_reader_reader_stop_all, rfid_multi_reader_rfid_multi_reader_rfid_stop_all, subghz_auto_recorder_subghz_auto_recorder_sub_rec_listen_stop [INFERRED 0.80]
- **CLI Log-Capture Debug Workflow (cap.py + live streams + power reboot recovery)** — claude_md_debug_loop, cap_py, readme_md [INFERRED 0.80]
- **Firmware-Evaluated Guard Instead Of Raw Enum/Struct Trust** — claude_md_fork_abi_drift, universal_card_reader_reader_nfc_reader_poll_protocol, universal_card_reader_card_info_dev_has, subghz_auto_recorder_subghz_auto_recorder_sub_rec_presets_self_check [INFERRED 0.75]

## Communities (35 total, 4 thin omitted)

### Community 0 - "Sub-GHz Recorder App State & Radio"
Cohesion: 0.07
Nodes (61): Fork ABI Drift (enum/struct layout not API-version-gated), GUI Thread Owns the Radio / View Model, One-time ethics notice gate before Listen, SubRecPreset fork-tail ABI headroom, Fork-stable preset id restriction (0-3 only), Generation-stamped event scheme, GUI thread owns the radio (invariant), Rolling-code detection and Replay warning (+53 more)

### Community 1 - "Universal Card Reader App State & LF/NFC Workers"
Cohesion: 0.09
Nodes (53): Async Worker Stop Is Not Synchronous, Generation-stamped stale-event filtering, ReaderApp struct, ReaderCardKind enum, ReaderCustomEvent enum, ReaderView enum, reader_lf_callback(), reader_lf_handle_read() (+45 more)

### Community 2 - "EMV TLV Parsing & Data Harvesting"
Cohesion: 0.1
Nodes (47): ber_len(), ber_tag(), dol_next(), emv_apdu(), emv_build_pdol_data(), emv_copy_name(), emv_gpo(), emv_harvest() (+39 more)

### Community 3 - "RFID Multi-Reader HF Backend"
Cohesion: 0.1
Nodes (45): dev_has(), hf_alloc(), hf_available(), hf_backend Vtable Instance, hf_card_name(), hf_describe(), hf_describe_felica(), hf_describe_iso14443_3a() (+37 more)

### Community 4 - "Repo-Level Dev Rules & Documentation"
Cohesion: 0.09
Nodes (37): CLAUDE.md — Flipper FAP Development Rules, Colliding Radios / Strict Stop-Free-Start Ordering, CLI Debug Loop (cap.py + live log/top streams), Generation-Stamped Custom Events, Mandatory Two-Pass Review (flipper-c-review + flipper-perf-review), Stack Sizing By Measurement, Not Guess, Step 0: Firmware/SDK Verification, EMV Read Diagnosis (universal_card_reader) (+29 more)

### Community 5 - "EMV APDU Read & Selection Flow"
Cohesion: 0.07
Nodes (24): dol_next(), emv_apdu(), emv_build_pdol_data(), emv_gpo(), emv_harvest(), emv_harvest_level(), emv_load_transport(), emv_parse_log_record() (+16 more)

### Community 6 - "Canvas Draw Helpers (Sub-GHz + Card Reader UI)"
Cohesion: 0.15
Nodes (28): clamp01(), draw_centered(), draw_centered_fit(), draw_listening(), draw_rssi_bar(), draw_sending(), draw_title_bar(), sub_rec_draw_callback() (+20 more)

### Community 7 - "Card Info Formatting & Display"
Cohesion: 0.25
Nodes (25): card_info_emv(), card_info_emv_identity(), card_info_emv_log(), card_info_felica(), card_info_format_emv(), card_info_format_lf(), card_info_format_nfc(), card_info_iso14443_3a() (+17 more)

### Community 8 - "EMV AID Database Load/Save"
Cohesion: 0.09
Nodes (9): card_info_emv(), card_info_emv_identity(), card_info_emv_log(), card_info_format_emv(), card_info_format_nfc(), format_emv_currency(), format_emv_date(), emv_load() (+1 more)

### Community 9 - "RFID Multi-Reader UI Draw Helpers"
Cohesion: 0.25
Nodes (15): RfidModel View Model Struct, draw_band(), draw_card_icon(), draw_centered(), draw_centered_fit(), draw_dots(), draw_radar(), draw_state_reading() (+7 more)

### Community 10 - "Universal Card Reader Packaging & Icon"
Cohesion: 0.5
Nodes (3): universal_card_reader application.fam manifest, universal_card_reader icon.png, Generate icon.png (10x10, 1-bit) for the Universal Card Reader FAP.  Design: a s

## Ambiguous Edges - Review These
- `rfid_navigation_callback()` → `universal_card_reader.c`  [AMBIGUOUS]
  rfid_multi_reader/rfid_multi_reader.c · relation: references
- `sub_rec_handle_notice_done()` → `sub_rec_custom_event_callback()`  [AMBIGUOUS]
  subghz_auto_recorder/subghz_auto_recorder.c · relation: calls
- `reader_app.h` → `RfidTimerRole Enum (Cross-Thread Timer Tag)`  [AMBIGUOUS]
  rfid_multi_reader/rfid_app.h · relation: references
- `reader_do_emulate()` → `Known firmware hang emulating loaded Mifare Classic/EMV cards`  [AMBIGUOUS]
  universal_card_reader/README.md · relation: rationale_for
- `Stack Sizing By Measurement, Not Guess` → `EMV Read Diagnosis (universal_card_reader)`  [AMBIGUOUS]
  doc/emv-read-diagnosis.md · relation: conceptually_related_to

## Knowledge Gaps
- **21 isolated node(s):** `Generate icon.png (10x10, 1-bit) for the RFID Multi-Reader FAP.  Design: an emit`, `Generate icon.png (10x10, 1-bit) for the SubGHz Auto Recorder FAP.  Design: a ve`, `Generate icon.png (10x10, 1-bit) for the Universal Card Reader FAP.  Design: a s`, `Generation-stamped event guard (poller vs GUI thread)`, `Firmware fork ABI compatibility (stable protocol ids)` (+16 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **4 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **What is the exact relationship between `rfid_navigation_callback()` and `universal_card_reader.c`?**
  _Edge tagged AMBIGUOUS (relation: references) - confidence is low._
- **What is the exact relationship between `sub_rec_handle_notice_done()` and `sub_rec_custom_event_callback()`?**
  _Edge tagged AMBIGUOUS (relation: calls) - confidence is low._
- **What is the exact relationship between `reader_app.h` and `RfidTimerRole Enum (Cross-Thread Timer Tag)`?**
  _Edge tagged AMBIGUOUS (relation: references) - confidence is low._
- **What is the exact relationship between `reader_do_emulate()` and `Known firmware hang emulating loaded Mifare Classic/EMV cards`?**
  _Edge tagged AMBIGUOUS (relation: rationale_for) - confidence is low._
- **What is the exact relationship between `Stack Sizing By Measurement, Not Guess` and `EMV Read Diagnosis (universal_card_reader)`?**
  _Edge tagged AMBIGUOUS (relation: conceptually_related_to) - confidence is low._
- **Why does `ReaderApp struct` connect `Universal Card Reader App State & LF/NFC Workers` to `EMV TLV Parsing & Data Harvesting`?**
  _High betweenness centrality (0.084) - this node is a cross-community bridge._
- **Why does `EMV Read Diagnosis (universal_card_reader)` connect `Repo-Level Dev Rules & Documentation` to `EMV TLV Parsing & Data Harvesting`?**
  _High betweenness centrality (0.053) - this node is a cross-community bridge._