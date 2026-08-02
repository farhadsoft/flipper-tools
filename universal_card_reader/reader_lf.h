#pragma once

#include "reader_app.h"

void reader_stop_lf(ReaderApp* app);
void reader_start_lf_phase(ReaderApp* app);
void reader_start_lf_emulation(ReaderApp* app);

// Handles ReaderEventLfRead. GUI thread only (stops the radio via reader_stop_all()).
void reader_lf_handle_read(ReaderApp* app);
