#pragma once

#include <stdbool.h>
#include <stddef.h>

// Splits one subghz decoder get_string() frame into the two pieces the
// listening screen shows: its first line ("<name> <bits>bit") and the hex of
// its "Key:" line. Both outputs are always NUL-terminated; `key` is "" when
// the frame carries no Key line.
//
// The Key line's shape is per-protocol, read off the firmware's own decoders:
// keeloq prints "Key:%08lX%08lX", princeton/came/nice_flo/linear print
// "Key:0x%lX". So a leading 0x is dropped and interior spaces (none today,
// but the .sub file format uses them) are skipped; everything else is copied
// verbatim until the line ends.
//
// HAL-free on purpose: this is the only parsing logic the live readout has,
// and test/test_recorder_parse.c pins it with known-answer vectors taken from
// those firmware format strings.
void sub_rec_live_parse(
    const char* txt, char* proto, size_t proto_size, char* key, size_t key_size);
