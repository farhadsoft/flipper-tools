#include "toolkit_log.h"

#include <flipper_format/flipper_format.h>

#define TOOLKIT_LOG_DIR  EXT_PATH("apps_data/universal_toolkit")
#define TOOLKIT_LOG_PATH EXT_PATH("apps_data/universal_toolkit/session.log")

// "%lu %u %s|%s" worst case: a uint32_t (10 digits) + ' ' + a uint8_t (3
// digits) + ' ' + a full summary + '|' + a full path + NUL.
#define TOOLKIT_LOG_LINE_MAX \
    (10 + 1 + 3 + 1 + (TOOLKIT_SUMMARY_MAX - 1) + 1 + (TOOLKIT_PATH_MAX - 1) + 1)

// storage_simply_mkdir() creates ONE level and returns true if the path
// already exists -- the firmware only guarantees /ext/apps_data itself, so
// both levels are created explicitly (same two-level pattern as
// universal_card_reader's reader_ensure_dirs() / subghz_auto_recorder's
// sub_rec_ensure_dir()).
static void toolkit_log_ensure_dir(ToolkitApp* app) {
    storage_simply_mkdir(app->storage, EXT_PATH("apps_data"));
    storage_simply_mkdir(app->storage, TOOLKIT_LOG_DIR);
}

bool toolkit_log_append(ToolkitApp* app, const ToolkitLogRecord* rec) {
    toolkit_log_ensure_dir(app);

    // "ts subsys summary|file": summary may itself contain spaces (e.g.
    // "gpio module opened"), so a reader takes the first two
    // whitespace-delimited tokens as ts/subsys and treats the remainder of
    // the line as "summary|file" -- '|' is not a character TextInput can
    // produce, so it safely separates summary from the trailing file path.
    char line[TOOLKIT_LOG_LINE_MAX];
    snprintf(
        line,
        sizeof(line),
        "%lu %u %s|%s",
        (unsigned long)rec->ts,
        (unsigned)rec->subsys,
        rec->summary,
        rec->file);

    // FSOM_OPEN_APPEND (flipper_format_file_open_append) creates the file if
    // missing and seeks to EOF, so this is the entire open path -- no
    // separate "does it exist" branch needed.
    FlipperFormat* ff = flipper_format_file_alloc(app->storage);
    bool ok = flipper_format_file_open_append(ff, TOOLKIT_LOG_PATH) &&
              flipper_format_write_string_cstr(ff, "Entry", line);
    flipper_format_file_close(ff);
    flipper_format_free(ff);

    if(!ok) {
        FURI_LOG_W(TAG, "log append failed: %s", TOOLKIT_LOG_PATH);
    }
    return ok;
}
