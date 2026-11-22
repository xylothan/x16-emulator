// Commander X16 Emulator — CRC-32 and SHA-256 of a block of memory.
//
// The standard algorithms, so a result can be compared with what any host tool
// says about the same bytes. Whole-machine state has x16_hash.h, which is
// neither of these.
#ifndef MEM_HASH_H
#define MEM_HASH_H

#include <stddef.h>
#include <stdint.h>

// The IEEE 802.3 CRC, as zlib and `crc32` compute it.
uint32_t mem_crc32(const uint8_t *data, size_t len);

void mem_sha256(const uint8_t *data, size_t len, uint8_t out[32]);

#endif
