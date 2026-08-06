#pragma once

#include "../../toolkit_app.h"

// Wraps universal_card_reader as a toolkit module: the app's own alloc/free
// and event/nav callbacks do the real work (see reader_app.h), this is just
// the ToolkitModule-shaped glue. See CLAUDE.md "Universal Toolkit".
void card_reader_enter(ToolkitApp* app);
void card_reader_exit(ToolkitApp* app);
bool card_reader_event(ToolkitApp* app, uint32_t id);
bool card_reader_nav(ToolkitApp* app);
