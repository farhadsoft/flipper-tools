#pragma once

#include "../../toolkit_app.h"

// Wraps rfid_multi_reader as a toolkit module: the app's own alloc/free and
// event/nav callbacks do the real work (see rfid_app.h), this is just the
// ToolkitModule-shaped glue. See CLAUDE.md "Universal Toolkit".
void rfid_multi_enter(ToolkitApp* app);
void rfid_multi_exit(ToolkitApp* app);
bool rfid_multi_event(ToolkitApp* app, uint32_t id);
bool rfid_multi_nav(ToolkitApp* app);
