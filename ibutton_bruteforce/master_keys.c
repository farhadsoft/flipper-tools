#include "master_keys.h"

#include <furi.h>
#include <string.h>
#include "crc8_dallas.h"

static bool brute_master_key_protocol_allowed(const char* name) {
    return strcmp(name, BRUTE_PROTOCOL_DALLAS) == 0 ||
           strcmp(name, BRUTE_PROTOCOL_DALLAS_GENERIC) == 0 ||
           strcmp(name, BRUTE_PROTOCOL_CYFRAL) == 0 ||
           strcmp(name, BRUTE_PROTOCOL_METAKOM) == 0;
}

/* Example / placeholder entries only.

   The real value of this app is a curated master-key table built from
   documented community sources (firmware-fork universal-key lists,
   intercom maintenance manuals, etc.). The entries below are deliberately
   trivial, clearly-labeled examples so the app self-checks and runs.

   Replace them with authorized keys before testing any reader you own or
   have explicit permission to test. */
MasterKey master_keys[] = {
    {
        .name = "Example DS1990 all-zero serial",
        .protocol_name = BRUTE_PROTOCOL_DALLAS,
        .protocol_id = -1,
        .data = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3D},
    },
    {
        .name = "Example DS1990 all-ones serial",
        .protocol_name = BRUTE_PROTOCOL_DALLAS,
        .protocol_id = -1,
        .data = {0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x2F},
    },
    {
        .name = "Example Cyfral 0x0000",
        .protocol_name = BRUTE_PROTOCOL_CYFRAL,
        .protocol_id = -1,
        .data = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    },
    {
        .name = "Example Cyfral 0xFFFF",
        .protocol_name = BRUTE_PROTOCOL_CYFRAL,
        .protocol_id = -1,
        .data = {0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    },
    {
        .name = "Example Metakom 0x00000000",
        .protocol_name = BRUTE_PROTOCOL_METAKOM,
        .protocol_id = -1,
        .data = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    },
    {
        .name = "Example Metakom 0xFFFFFFFF",
        .protocol_name = BRUTE_PROTOCOL_METAKOM,
        .protocol_id = -1,
        .data = {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00},
    },
};

size_t master_keys_count = COUNT_OF(master_keys);

bool brute_master_keys_self_check(iButtonProtocols* protocols) {
    furi_check(protocols);

    if(master_keys_count == 0) {
        FURI_LOG_E("Brute", "master_keys table is empty");
        return false;
    }

    for(size_t i = 0; i < master_keys_count; ++i) {
        MasterKey* key = &master_keys[i];

        if(!brute_master_key_protocol_allowed(key->protocol_name)) {
            FURI_LOG_E("Brute", "master key %d: protocol not in whitelist: %s", i, key->protocol_name);
            return false;
        }

        key->protocol_id = ibutton_protocols_get_id_by_name(protocols, key->protocol_name);
        if(key->protocol_id == iButtonProtocolIdInvalid) {
            FURI_LOG_E("Brute", "master key %d: unknown protocol %s", i, key->protocol_name);
            return false;
        }

        if(strcmp(key->protocol_name, BRUTE_PROTOCOL_DALLAS) == 0) {
            const uint8_t crc = crc8_dallas(key->data, 7);
            if(crc != key->data[7]) {
                FURI_LOG_E(
                    "Brute",
                    "master key %d: CRC mismatch: got %02X, want %02X",
                    i,
                    key->data[7],
                    crc);
                return false;
            }
        }
    }

    FURI_LOG_I("Brute", "master_keys self-check passed, %zu entries", master_keys_count);
    return true;
}
