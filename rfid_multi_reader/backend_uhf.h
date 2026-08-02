#pragma once

// rfid_backend_uhf() is declared in rfid_backend.h; this header only pulls
// that in so backend_uhf.c has a matching self-include, matching the .c/.h
// pairing of every other module here. Re-declaring it here would trip
// -Werror=redundant-decls.
#include "rfid_backend.h"
