/* ==========================================================================
 * usn_wire_crc.h -- CRC32C (Castagnoli) used by the Universal Sniffer wire
 *                   protocol for both the packet header and the packet body.
 *
 * C-compatible and shared by desktop and firmware. The implementation lives in
 * usn_wire_crc.c so there is exactly ONE copy in the whole product: firmware
 * and host therefore cannot disagree about integrity checking.
 *
 * Honest note on what this CRC is for: USB bulk transfers are already reliable
 * (the USB protocol has its own per-packet CRC and retransmission), so this
 * does not protect against wire bit-rot. It protects against DMA/buffer bugs
 * that tear or duplicate samples, host read/write races that split packets at
 * unexpected boundaries, and framer logic errors. The integrity story rests on
 * four legs in priority order: sequence numbers, length validation, magic-based
 * resynchronisation, then CRC (docs/architecture_review.md section 8.1).
 * ========================================================================== */

#ifndef USN_WIRE_CRC_H
#define USN_WIRE_CRC_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CRC32C initial value. */
#define USN_CRC32C_INIT 0xFFFFFFFFu

/* Streaming update. Pass USN_CRC32C_INIT as the first crc, then chain. */
uint32_t usn_crc32c_update(uint32_t crc, const void* data, size_t length);

/* Complete the computation (final xor). */
uint32_t usn_crc32c_final(uint32_t crc);

/* One-shot convenience: CRC32C over a whole buffer. */
uint32_t usn_crc32c(const void* data, size_t length);

/* Verify a buffer against an expected final CRC. Returns 1 on match. */
int usn_crc32c_verify(const void* data, size_t length, uint32_t expected);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* USN_WIRE_CRC_H */
