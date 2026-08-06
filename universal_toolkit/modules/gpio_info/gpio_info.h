#pragma once

#include "../../toolkit_app.h"

// Phase 0 proof module: reads the 8 external GPIO header pins on a 200 ms
// timer. No radio, no USB-mode change -- the safest possible module to
// prove the lifecycle contract against. See CLAUDE.md "Universal Toolkit".
void gpio_info_enter(ToolkitApp* app);
void gpio_info_exit(ToolkitApp* app);
bool gpio_info_event(ToolkitApp* app, uint32_t id);
bool gpio_info_nav(ToolkitApp* app);
