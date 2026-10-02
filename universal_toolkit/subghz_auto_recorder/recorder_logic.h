#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// The module's HAL-free logic, pinned by test/test_recorder_logic.c:
//
// sub_rec_live_parse() splits one subghz decoder get_string() frame into the
// two pieces the listening screen shows: its first line ("<name> <bits>bit")
// and the hex of its "Key:" line. Both outputs are always NUL-terminated;
// `key` is "" when the frame carries no Key line. The Key line's shape is
// per-protocol, read off the firmware's own decoders: keeloq prints
// "Key:%08lX%08lX", princeton/came/nice_flo/linear print "Key:0x%lX". So a
// leading 0x is dropped and interior spaces (none today, but the .sub file
// format uses them) are skipped; everything else is copied verbatim until
// the line ends.
//
// sub_rec_hopper_decide() is the hopper's dwell state machine at stock
// subghz_txrx_hopper_update() parity, extracted because getting it wrong
// pins the radio to one frequency forever -- which is exactly what the
// first version of it did.
void sub_rec_live_parse(
    const char* txt, char* proto, size_t proto_size, char* key, size_t key_size);

// One hopper decision. `above` is whether the current frequency's RSSI is
// above HOPPER_RSSI_FLOOR; it is only consulted when not dwelling, matching
// stock, which does not read RSSI on the tick the dwell countdown expires.
// Returns true when the caller must advance to the next hopper frequency:
// immediately when the band is quiet, and once -- unconditionally -- when a
// `dwell_ticks` countdown started by a loud frequency runs out, so a
// continuous carrier cannot hold the radio in place.
// dwell_ticks is a parameter, not a read of recorder_app.h's constant: this
// file must stay includable by test/ with no Flipper SDK on the path.
bool sub_rec_hopper_decide(bool* dwell, uint8_t* timeout, uint8_t dwell_ticks, bool above);
