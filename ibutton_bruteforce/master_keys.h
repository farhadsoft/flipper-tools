#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <ibutton/ibutton_protocols.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Protocol names are resolved at runtime, so enum drift between forks cannot bite us. */
#define BRUTE_PROTOCOL_DALLAS "DS1990"
#define BRUTE_PROTOCOL_CYFRAL "Cyfral"
#define BRUTE_PROTOCOL_METAKOM "Metakom"
#define BRUTE_PROTOCOL_DALLAS_GENERIC "DSGeneric"

typedef struct {
    const char* name; /* What the key is documented for. */
    const char* protocol_name;
    iButtonProtocolId protocol_id; /* Filled by brute_master_keys_self_check(). */
    uint8_t data[8];
} MasterKey;

/* Curated table of documented community master keys. The default table ships
   with only a few clearly-labeled example entries so the app self-checks and
   runs out of the box; replace them with keys from documented, authorized
   sources before any real use. */
extern MasterKey master_keys[];
extern size_t master_keys_count;

/* Returns true if the table is valid and every protocol name resolved. */
bool brute_master_keys_self_check(iButtonProtocols* protocols);

#ifdef __cplusplus
}
#endif
