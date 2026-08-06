# iButton Brute Force: Sequential/Master Mode Effectiveness — Design

**Status:** Approved. **Date:** 2026-08-06. **App:** `ibutton_bruteforce/` (single commit `7b6453f`, no prior spec).

## Background

`ibutton_bruteforce` emulates a curated master-key table and runs a bounded
sequential walk against an iButton/1-Wire reader (`ibutton_bruteforce.c:374-428`
tick state machine; `brute_worker.c` owns all `ibutton_worker_*` calls).

### De Bruijn sequence — investigated, ruled out

Question: can overlapping key presentations (De Bruijn-style) cut total walk
time, the way it does for shift-register SubGHz fixed-code receivers
(Samy Kamkar's OpenSesame: 98304 bits → 4105 bits for a 12-bit code)?

**No — 1-Wire is a discrete, framed protocol, not a continuous bitstream.**
Every key presentation requires a bus reset, presence pulse, ROM command, and
an 8-byte ID transmission with a Dallas CRC8 tail
(`crc8_dallas`, `ibutton_bruteforce.c:242`). The app's own worker already
enforces one full transaction per attempt — `brute_worker_emulate_key()` calls
`ibutton_worker_stop()` then `emulate_start()` per key (`brute_worker.c:146-164`).
There is no shared window for a reader to slide over; the reader paces itself
via its own bus resets. Confirmed further against the general SubGHz case: even
where De Bruijn is theoretically valid, the most-used community implementation
(`tobiabocchi/flipperzero-bruteforce`, 2.5k★) documents that its `debruijn.sub`
files "have not really been able to be used successfully... most protocols
have some sort of stop bit or pilot bit" — the technique needs a true
shift-register receiver, which doesn't exist in this app's target hardware
class either. Not revisiting this; it's a dead end for iButton specifically
and mostly a dead end for the SubGHz case that motivates it.

### Real findings, from reading the run loop

1. **Cyfral sequential mode has a real bug.** `brute_generate_cyfral_key`
   (`ibutton_bruteforce.c:245-248`) only uses the low 16 bits of `index` (Cyfral
   is a 2-byte key — full keyspace `2^16 = 65536`), but `total_keys` is
   hardcoded to `BRUTE_SEQ_TOTAL` (100000) for every protocol
   (`ibutton_bruteforce.c:324`). Indices 65536-99999 silently re-present keys
   0-34463 a second time — roughly 35% of the run wastes dwell/gap budget on
   duplicates, and the run never reports genuine exhaustive coverage even
   though the full keyspace fits inside the existing budget.
2. **Family code is not exposed in Settings.** `app->family` only ever comes
   from `BRUTE_SEQ_FAMILY_DEFAULT` (0x01) or a hand-edited `progress.txt` —
   `BruteSettingItem` has no Family row. It's already fully persisted
   (`brute_progress_save` line 532, `brute_progress_load` line 467) — only the
   UI control is missing.

These reframe the original "prioritize common family codes" idea: there's no
real prevalence data across multiple Dallas family codes for this hardware
class to justify a weighted-priority list (and inventing one would be
fabrication). What's actually broken is more basic — the control doesn't
exist, and Cyfral's bound is wrong.

## Approaches considered

1. **Correctness-first, minimal (chosen).** Master keys move to an SD file;
   fix Cyfral's total to the real keyspace; add one Family setting. One
   family/value per run, same interaction model as Protocol/Dwell/Gap today.
2. **Multi-family queue.** All of (1), plus family becomes an ordered list
   that sequential mode auto-advances through in one unattended run.
   Bigger state-machine change (`current_index` becomes a family+serial pair;
   progress/resume and status UI both need both fields). Deferred — no
   evidence yet that unattended multi-family runs are needed in practice.
3. **Data-driven target profiles.** Master keys + sequential config unified
   into named, savable presets per reader. Rejected as over-engineered for a
   personal tool absent a demonstrated need to cycle between many reader
   profiles.

## Design

### 1. Master-key table: SD file, not compiled-in

Sourcing boundary: the table is **not** populated from scraped web content —
the search results for "domophone master keys" are unverified-provenance
bypass-code blogs, not the "documented, authorized sources" this app's own
README already commits to. The design below builds a clean, validated,
user-editable format; the user populates it from sources they trust. The
existing fail-closed self-check is the right mechanism to keep that honest and
carries forward unchanged in spirit.

- **Location:** `/ext/apps_data/ibutton_bruteforce/master_keys.txt` (same
  folder as `progress.txt`).
- **Format:** FlipperFormat, repeated-record style — the same idiom the
  firmware already uses for repeated same-key lines (e.g. `RAW_Data:`):
  read a key name in a loop until the read fails, rather than random access.
  ```
  Filetype: IBF Master Keys
  Version: 1
  Name: Example DS1990 all-zero serial
  Protocol: DS1990
  Data: 01 00 00 00 00 00 00 3D
  Name: Example Cyfral 0x0000
  Protocol: Cyfral
  Data: 00 00
  ```
  `Data:` byte count is implied by `Protocol:` (DS1990=8, Cyfral=2,
  Metakom=4) — human-editable, no zero-padding noise in the file. Internally
  still lands in a fixed `uint8_t data[8]`, zero-padded past the protocol's
  real length (matches how `brute_worker_emulate_key`'s
  `min(data_size, editable.size)` clamp already treats it today).
- **Loading:** replaces the static initializer in `master_keys.c`. Runs where
  `brute_master_keys_self_check()` runs today, inside `brute_worker_setup()`
  — `app->protocols` is already allocated by that point, so protocol-name to
  id resolution is unchanged. Single-pass parse with a doubling-growth array
  (no rewind-semantics or double-open ambiguity).
- **First run (file missing):** write the file from a small compiled-in seed
  (the current six example entries, unchanged), then read it back through the
  same parser used forever after — one code path, not a compiled-path vs
  file-path fork.
- **Existing globals** `master_keys[]`/`master_keys_count` become
  heap-allocated (`MasterKey*`), populated by `brute_master_keys_load()`,
  released by a new `brute_master_keys_free()` called from
  `brute_worker_teardown()`. `master_keys.c` stays the sole file owning
  key-table concerns.
- **`MasterKey.protocol_name` is dropped** — nothing outside the load-time
  validation ever reads it (confirmed: `brute_load_master_key` only touches
  `protocol_id`, `data`, `name`); it becomes a transient local during parsing.

### 2. Sequential mode: protocol-aware total + Family control

- **Cyfral exhaustive fix:** `brute_run_start()`'s
  `app->total_keys = BRUTE_SEQ_TOTAL` becomes protocol-aware — Cyfral gets
  exactly `65536` (the full keyspace; small enough there's no reason to bound
  it), Dallas/Metakom keep the unchanged `BRUTE_SEQ_TOTAL` (100000) bounded
  default.
- **Family setting:** new `BruteSettingFamily` row, right after Protocol.
  Reuses the existing NumberInput flow Start Index already uses (0-255).
  Since both Family and Start Index share that one number-entry screen, the
  app needs to remember which one it opened for — one new
  `BruteSettingItem number_input_target` field routes the result back to the
  right field. Shown unconditionally, including when Protocol is Cyfral or
  Metakom (whose generators ignore it) — avoids rebuilding the settings list
  on protocol change for no real benefit; inert is fine, wrong is not.
- **Not touched:** the pre-existing behavior where `start_index >= total_keys`
  presents exactly one key before reporting Done — unchanged by this design,
  not introduced by it, out of scope for "minimal." Also not touched: the
  pre-existing staleness where a settings row's displayed value text doesn't
  refresh after a NumberInput edit until the app restarts (Start Index has
  this today; Family will inherit the identical, already-accepted behavior
  rather than silently diverging).

### 3. Error handling

Unchanged philosophy: a malformed *existing* file (bad protocol name, Dallas
CRC mismatch, unreadable) fails closed — app refuses to start, same
`master_keys self-check` log line the README's Troubleshooting section
already documents. Only a *missing* file auto-seeds; a *broken* one never
silently drops entries or falls back to defaults.

### 4. Testing / verification

No unit test framework in this codebase (device-verified by convention). Plan
covers on-device checks: fresh SD (no `master_keys.txt`) creates and loads the
seed; hand-edited file with a bad CRC refuses to start with the expected log
line; Cyfral sequential run reaches index 65535 and reports Done without
wrapping; Family setting round-trips through Settings → run → `progress.txt`
→ relaunch.

## Non-goals

- Multi-family queue / auto-advancing sequential run (approach 2, deferred).
- Named target profiles (approach 3, rejected as over-engineered).
- Populating the master-key table with real-world codes — infrastructure
  only; the user supplies data from sources they trust.
- Fixing the pre-existing `start_index >= total_keys` and stale-settings-label
  behaviors — both predate this design and are out of scope for it.
