#include "recorder_parse.h"

#include <string.h>

void sub_rec_live_parse(
    const char* txt, char* proto, size_t proto_size, char* key, size_t key_size) {
    if(proto_size) {
        proto[0] = '\0';
        if(txt) {
            // Line 1 ends at the first CR; a frame without one is all line 1.
            const char* cr = strchr(txt, '\r');
            size_t n = cr ? (size_t)(cr - txt) : strlen(txt);
            if(n >= proto_size) n = proto_size - 1;
            memcpy(proto, txt, n);
            proto[n] = '\0';
        }
    }

    if(key_size) {
        key[0] = '\0';
        const char* kp = txt ? strstr(txt, "Key:") : NULL;
        if(kp) {
            kp += 4;
            if(kp[0] == '0' && (kp[1] == 'x' || kp[1] == 'X')) kp += 2;
            size_t w = 0;
            for(; *kp && *kp != '\r' && *kp != '\n' && w + 1 < key_size; kp++) {
                if(*kp == ' ') continue;
                key[w++] = *kp;
            }
            key[w] = '\0';
        }
    }
}
