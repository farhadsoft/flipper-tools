#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Dallas/1-Wire CRC8 (poly 0x31, LSB-first). Computed over buf[0..len-1]. */
static inline uint8_t crc8_dallas(const uint8_t* buf, size_t len) {
    uint8_t crc = 0;
    for(size_t i = 0; i < len; ++i) {
        crc ^= buf[i];
        for(uint8_t bit = 0; bit < 8; ++bit) {
            if(crc & 1U) {
                crc = (crc >> 1) ^ 0x8CU; /* 0x31 reflected */
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

#ifdef __cplusplus
}
#endif
