/**
 * @file crc32.c
 * @brief IEEE 802.3 CRC-32, Castagnoli CRC-32C, and Adler-32 Checksum Implementation
 */

#include "crc32.h"

// Polynomials:
// IEEE 802.3 CRC-32: 0xEDB88320
// Castagnoli CRC-32C: 0x82F63B78

uint32_t crc32_calc(const uint8_t *data, size_t len) {
    if (!data) return 0;
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
        }
    }
    return ~crc;
}

uint32_t crc32c_calc(const uint8_t *data, size_t len) {
    if (!data) return 0;
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0x82F63B78 : 0);
        }
    }
    return ~crc;
}

uint32_t adler32_calc(const uint8_t *data, size_t len) {
    if (!data) return 1;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; i++) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}
