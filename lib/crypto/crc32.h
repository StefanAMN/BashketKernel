/**
 * @file crc32.h
 * @brief IEEE 802.3 CRC-32, Castagnoli CRC-32C, and Adler-32 Checksum Interface
 */

#ifndef LIB_CRYPTO_CRC32_H
#define LIB_CRYPTO_CRC32_H

#include <stddef.h>
#include <stdint.h>

uint32_t crc32_calc(const uint8_t *data, size_t len);
uint32_t crc32c_calc(const uint8_t *data, size_t len);
uint32_t adler32_calc(const uint8_t *data, size_t len);

#endif // LIB_CRYPTO_CRC32_H
