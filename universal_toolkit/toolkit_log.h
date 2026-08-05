#pragma once

#include "toolkit_app.h"

// Appends one record to /ext/apps_data/universal_toolkit/session.log as a
// FlipperFormat "Entry: ts subsys summary|file" line, creating the app data
// dir on first use. Every module calls this (entry, save, ...); logs a
// warning and returns false on failure rather than claiming success blindly.
bool toolkit_log_append(ToolkitApp* app, const ToolkitLogRecord* rec);
