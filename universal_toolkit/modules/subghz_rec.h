#pragma once

#include "../toolkit_app.h"

// Wraps subghz_auto_recorder as a toolkit module. See CLAUDE.md "Universal
// Toolkit" -- Phase 1.
void subghz_rec_enter(ToolkitApp* app);
void subghz_rec_exit(ToolkitApp* app);
bool subghz_rec_event(ToolkitApp* app, uint32_t id);
bool subghz_rec_nav(ToolkitApp* app);
