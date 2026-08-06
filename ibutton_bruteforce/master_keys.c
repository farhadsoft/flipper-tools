#include "master_keys.h"

#include <furi.h>
#include <string.h>
#include <storage/storage.h>
#include <lib/flipper_format/flipper_format.h>

#include "brute_app.h" /* BRUTE_APP_FOLDER */
#include "crc8_dallas.h"

#define BRUTE_MASTER_KEYS_FILE BRUTE_APP_FOLDER "/master_keys.txt"
#define BRUTE_MASTER_KEYS_INITIAL_CAP 8U

MasterKey* master_keys = NULL;
size_t master_keys_count = 0;

/* Seed written to master_keys.txt the first time the app runs on a card that doesn't
   have one yet -- the same six example entries the static table used to ship with.
   Deliberately trivial, clearly-labeled placeholders: replace with keys from
   documented, authorized sources before any real use. */
typedef struct {
    const char* name;
    const char* protocol_name;
    uint8_t data[8];
    uint8_t data_len;
} MasterKeySeed;

static const MasterKeySeed master_keys_seed[] = {
    {"Example DS1990 all-zero serial",
     BRUTE_PROTOCOL_DALLAS,
     {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3D},
     8},
    {"Example DS1990 all-ones serial",
     BRUTE_PROTOCOL_DALLAS,
     {0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x2F},
     8},
    {"Example Cyfral 0x0000", BRUTE_PROTOCOL_CYFRAL, {0x00, 0x00, 0, 0, 0, 0, 0, 0}, 2},
    {"Example Cyfral 0xFFFF", BRUTE_PROTOCOL_CYFRAL, {0xFF, 0xFF, 0, 0, 0, 0, 0, 0}, 2},
    {"Example Metakom 0x00000000",
     BRUTE_PROTOCOL_METAKOM,
     {0x00, 0x00, 0x00, 0x00, 0, 0, 0, 0},
     4},
    {"Example Metakom 0xFFFFFFFF",
     BRUTE_PROTOCOL_METAKOM,
     {0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0},
     4},
};

/* Returns the on-wire byte count for a protocol name, or 0 if not recognized --
   0 doubles as the "not in whitelist" signal for brute_master_keys_parse(). */
static uint8_t brute_protocol_data_len(const char* protocol_name) {
    if(strcmp(protocol_name, BRUTE_PROTOCOL_DALLAS) == 0) return 8;
    if(strcmp(protocol_name, BRUTE_PROTOCOL_CYFRAL) == 0) return 2;
    if(strcmp(protocol_name, BRUTE_PROTOCOL_METAKOM) == 0) return 4;
    return 0;
}

static void brute_master_keys_write_seed(Storage* storage) {
    storage_common_mkdir(storage, BRUTE_APP_FOLDER);

    FlipperFormat* ff = flipper_format_buffered_file_alloc(storage);
    do {
        if(!flipper_format_buffered_file_open_always(ff, BRUTE_MASTER_KEYS_FILE)) break;
        if(!flipper_format_write_header_cstr(ff, "IBF Master Keys", 1)) break;

        bool ok = true;
        for(size_t i = 0; i < COUNT_OF(master_keys_seed) && ok; ++i) {
            const MasterKeySeed* seed = &master_keys_seed[i];
            ok = flipper_format_write_string_cstr(ff, "Name", seed->name) &&
                 flipper_format_write_string_cstr(ff, "Protocol", seed->protocol_name) &&
                 flipper_format_write_hex(ff, "Data", seed->data, seed->data_len);
        }
        if(!ok) {
            FURI_LOG_E("Brute", "master_keys.txt: failed writing seed");
        }
    } while(false);
    flipper_format_free(ff);
}

/* Single-pass parse into a doubling-growth array -- avoids both a two-pass
   count-then-rewind (rewind-to-where is ambiguous without firmware source) and
   opening the file twice. Fails closed: stops at the first bad record. */
static bool brute_master_keys_parse(FlipperFormat* ff, iButtonProtocols* protocols) {
    FuriString* name = furi_string_alloc();
    FuriString* protocol = furi_string_alloc();

    size_t cap = BRUTE_MASTER_KEYS_INITIAL_CAP;
    MasterKey* keys = malloc(cap * sizeof(MasterKey));
    size_t count = 0;
    bool ok = true;

    while(flipper_format_read_string(ff, "Name", name)) {
        if(count == cap) {
            cap *= 2;
            keys = realloc(keys, cap * sizeof(MasterKey));
        }
        MasterKey* key = &keys[count];
        snprintf(key->name, sizeof(key->name), "%s", furi_string_get_cstr(name));

        if(!flipper_format_read_string(ff, "Protocol", protocol)) {
            FURI_LOG_E("Brute", "master key %zu: missing Protocol", count);
            ok = false;
            break;
        }
        const char* protocol_cstr = furi_string_get_cstr(protocol);
        const uint8_t data_len = brute_protocol_data_len(protocol_cstr);
        if(data_len == 0) {
            FURI_LOG_E(
                "Brute", "master key %zu: protocol not in whitelist: %s", count, protocol_cstr);
            ok = false;
            break;
        }

        key->protocol_id = ibutton_protocols_get_id_by_name(protocols, protocol_cstr);
        if(key->protocol_id == iButtonProtocolIdInvalid) {
            FURI_LOG_E("Brute", "master key %zu: unknown protocol %s", count, protocol_cstr);
            ok = false;
            break;
        }

        memset(key->data, 0, sizeof(key->data));
        if(!flipper_format_read_hex(ff, "Data", key->data, data_len)) {
            FURI_LOG_E("Brute", "master key %zu: missing/short Data", count);
            ok = false;
            break;
        }

        if(strcmp(protocol_cstr, BRUTE_PROTOCOL_DALLAS) == 0) {
            const uint8_t crc = crc8_dallas(key->data, 7);
            if(crc != key->data[7]) {
                FURI_LOG_E(
                    "Brute",
                    "master key %zu: CRC mismatch: got %02X, want %02X",
                    count,
                    key->data[7],
                    crc);
                ok = false;
                break;
            }
        }

        count++;
    }

    furi_string_free(name);
    furi_string_free(protocol);

    if(!ok) {
        free(keys);
        return false;
    }
    if(count == 0) {
        FURI_LOG_E("Brute", "master_keys.txt: no entries");
        free(keys);
        return false;
    }

    free(master_keys);
    master_keys = keys;
    master_keys_count = count;
    return true;
}

bool brute_master_keys_load(Storage* storage, iButtonProtocols* protocols) {
    furi_check(storage);
    furi_check(protocols);

    if(!storage_file_exists(storage, BRUTE_MASTER_KEYS_FILE)) {
        FURI_LOG_I("Brute", "master_keys.txt missing, writing seed");
        brute_master_keys_write_seed(storage);
    }

    FlipperFormat* ff = flipper_format_buffered_file_alloc(storage);
    bool ok = false;

    do {
        if(!flipper_format_buffered_file_open_existing(ff, BRUTE_MASTER_KEYS_FILE)) {
            FURI_LOG_E("Brute", "master_keys.txt: cannot open");
            break;
        }

        FuriString* filetype = furi_string_alloc();
        uint32_t version = 0;
        const bool header_ok = flipper_format_read_header(ff, filetype, &version);
        const bool header_match =
            header_ok && furi_string_equal_str(filetype, "IBF Master Keys") && version == 1;
        furi_string_free(filetype);
        if(!header_match) {
            FURI_LOG_E("Brute", "master_keys.txt: bad header");
            break;
        }

        ok = brute_master_keys_parse(ff, protocols);
    } while(false);

    flipper_format_free(ff);

    if(ok) {
        FURI_LOG_I("Brute", "master_keys self-check passed, %zu entries", master_keys_count);
    } else {
        FURI_LOG_E("Brute", "master_keys self-check failed");
    }
    return ok;
}

void brute_master_keys_free(void) {
    free(master_keys);
    master_keys = NULL;
    master_keys_count = 0;
}
