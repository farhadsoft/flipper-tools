#pragma once

// rfid_backend_hf() is declared in rfid_backend.h (the single shared vtable
// contract); this header only pulls that in so backend_hf.c has a matching
// self-include, matching the .c/.h pairing of every other module here.
// Re-declaring it here would trip -Werror=redundant-decls.
#include "rfid_backend.h"
