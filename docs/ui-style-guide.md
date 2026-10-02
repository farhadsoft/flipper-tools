# UI style guide — Mono design system

Governs every **custom-drawn** screen in the repo's FAPs (the `View` bodies in
`*_ui.c` / `ui.c` files). Native widgets — TextBox reports, Submenu,
VariableItemList, NumberInput, TextInput — are not covered here and stay as-is.

The design system lives in `universal_toolkit/toolkit_ui.{h,c}`. Screens
consume it; they never fork it. No new magic pixel constants: everything comes
from the tokens below or the helper functions.

## Tokens (`toolkit_ui.h`)

|Token|Value|Meaning|
|---|---|---|
|`UI_W` / `UI_H`|128 / 64|Canvas size|
|`UI_STATUSBAR_H`|12|Inverted status bar occupies rows 0..11|
|`UI_RULE_Y`|12|Horizontal rule under the status bar|
|`UI_CONTENT_TOP`|15|Body content starts here|
|`UI_FOOTER_Y`|51|Footer row baseline area|
|`UI_MARGIN`|4|Left margin; right edge is `UI_W - UI_MARGIN`|
|`UI_MARGIN_WIDE`|6|Wider inset for de-emphasized rows|
|`UI_METER_H` / `UI_METER_SEG`|6 / 8|Meter bar height / segment pitch|

## Status bar (`ui_status_bar`)

- **Left label = short state name in mixed case** — `Scan`, `Reading`,
  `Emulating`, `Listening`, `Sending`, `Info`, `Stats`, `Wave <zoom>`. Never
  the module name. ALL-CAPS only for `RECORDING` (deliberate emphasis,
  shipped).
- **Glyph flags**: active radio sweep → `UiStatusScanning`; recording capture →
  `UiStatusRecording`; ongoing radio/live work without its own glyph (Reading,
  Emulating, GPIO live) → `UiStatusLive`; BLE → `UiStatusBle`; static screens
  (Sending, Stats, Analyze, Notice) → `0`.
- **`phase`** = the screen's existing animation counter (`frame` /
  `anim_phase`) when a glyph or meter pulses, else `0`.
- **`battery`** = `m->battery` from the model — **never a literal `0`** (0
  draws an empty battery outline). Values >100 hide the icon (no current
  screen does this).

## Battery refresh rule

`m->battery` is filled with `furi_hal_power_get_pct()` **inside the existing
GUI-thread model setters** — on every animation bump
(`reader_bump_frame` / `rfid_bump_frame`, `sub_rec_set_rssi` update branch)
and on every screen-entry setter (`sub_rec_set_state`, `sub_rec_set_stats`,
`sub_rec_set_analyze`). Minutes-scale staleness on static screens is accepted.
Never call it from a draw callback or a TimersSrv callback.

## Key hints

- **Grammar `<Key>=<action>`** with short key names: `OK`, `Back`, `v`, `^`.
  Examples: `OK=load`, `Back=stop`, `v=wave`, `^=info`, `OK=tune`.
- **Placement**: status-bar **center label** when left+center fit —
  `ui_status_bar` silently drops the *left* label on collision, so only pair a
  center hint with a left label ≤5 chars. Otherwise bottom line y63 via
  `ui_draw_centered`.
- Only non-universal keys get a hint. `Back`=exit/menu is universal and never
  shown; `Back=stop` on Emulating *is* shown because it is an action, not
  navigation.

## Notice pattern

One `ui_status_bar(canvas, notice_title, NULL, 0, m->battery, 0)` then
`ui_notice(canvas, NULL, l1, l2)` then `return` — **before** the state switch,
so the bar draws exactly once per frame. The title lives in the bar, so
`ui_notice`'s own title argument is always `NULL`.

## Bodies

- Hero (`ui_hero`) + chip (`ui_chip`) + meter (`ui_meter`) for screens whose
  primary datum is numeric.
- Radio-motif art (radar arcs, wave rings, card icon) may carry scan/emulate
  screens.
- Emphasize a band/tag with an rframe box or a chip — never with `< ... >`
  text brackets.
- Centered lines via `ui_draw_centered` / `ui_draw_centered_fit` (fit max 124
  for full-width body lines, 100 inside the band box); left-aligned at
  x=`UI_MARGIN`; right-aligned at x=`UI_W - UI_MARGIN - width`.
- Session counters only via `ui_footer_counts`.

## Redraw

The 2026-10 UI consistency pass changed no `with_view_model` update flag and
no timer, and new screens must not either without a perf-review note.

## New-screen checklist

1. State-name left label (mixed case, not the module name).
2. Correct glyph flag + `phase` wiring.
3. Real battery from `m->battery` (setter-filled, never literal 0).
4. Key hints in `<Key>=<action>` grammar, placement per the collision rule.
5. Notice path: single `ui_status_bar` + `ui_notice` + early `return`.
6. Margins from tokens, no new magic pixel constants.
