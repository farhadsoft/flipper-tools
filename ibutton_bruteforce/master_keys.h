#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <storage/storage.h>
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
    char name[40]; /* Human label, truncated if larger. */
    iButtonProtocolId protocol_id; /* Resolved at load time. */
    uint8_t data[8]; /* Zero-padded past the protocol's real length. */
} MasterKey;

/* Curated table of documented master keys, loaded from
   /ext/apps_data/ibutton_bruteforce/master_keys.txt. If the file is missing, a small
   seed of clearly-labeled example entries is written first so the app runs out of the
   box; replace them with keys from documented, authorized sources before any real use.
   Heap-allocated by brute_master_keys_load(); release with brute_master_keys_free(). */
extern MasterKey* master_keys;
extern size_t master_keys_count;

/* Seeds the file if missing, then loads, parses, and validates every entry --
   protocol whitelist, protocol name resolution, Dallas CRC8. Fails closed: on any
   error master_keys/master_keys_count are left NULL/0 and false is returned. */
bool brute_master_keys_load(Storage* storage, iButtonProtocols* protocols);

/* Releases the table. Safe to call even if load was never called or failed. */
void brute_master_keys_free(void);

#ifdef __cplusplus
}
#endif
