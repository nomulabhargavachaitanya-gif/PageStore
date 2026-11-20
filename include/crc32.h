#ifndef PS_CRC32_H
#define PS_CRC32_H

#include <stddef.h>
#include <stdint.h>

/* CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320). */
uint32_t crc32_update(uint32_t crc, const void *data, size_t len);
uint32_t crc32_compute(const void *data, size_t len);

#endif
