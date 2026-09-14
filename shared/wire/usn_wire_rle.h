/* ==========================================================================
 * usn_wire_rle.h -- run-length codec for USN_PKT_SAMPLE_BLOCK_RLE.
 *
 * Protocol captures spend most of their time idle, so RLE typically buys one to
 * three orders of magnitude in effective capture duration for the same USB
 * bandwidth. That makes it the highest-leverage mitigation for the USB
 * throughput ceiling (docs/architecture_review.md sections 8.5 and 16.2 L5).
 *
 * The reference implementation is here, in C, so the firmware encoder and the
 * host decoder are the SAME code and cannot disagree.
 *
 * Return convention: 0 on success, negative on failure. These functions never
 * allocate and never read or write outside the caller-provided bounds; the
 * caller supplies capacities and the codec refuses to exceed them.
 * ========================================================================== */

#ifndef USN_WIRE_RLE_H
#define USN_WIRE_RLE_H

#include <stdint.h>
#include <stddef.h>
#include "usn_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes returned by the codec. */
enum UsnRleStatus {
    USN_RLE_OK              =  0,
    USN_RLE_ERR_ARGUMENT    = -1, /* null pointer, bad stride, zero count */
    USN_RLE_ERR_OUTPUT_FULL = -2, /* destination capacity insufficient   */
    USN_RLE_ERR_COUNT       = -3  /* decodedSampleCount mismatch         */
};

/* Worst case: every sample differs from its neighbour. */
static inline uint32_t usn_rle_max_runs(uint32_t sampleCount) { return sampleCount; }

/* Read/write one sample word at the given stride, little-endian.
 * strideBytes must be 1, 2 or 4; otherwise 0 is read and writes are ignored.
 * These are the only places in the product that interpret packed samples, so
 * the stride handling cannot drift between modules. */
uint32_t usn_wire_read_word(const void* samples, uint32_t index, uint8_t strideBytes);
void usn_wire_write_word(void* samples, uint32_t index, uint8_t strideBytes, uint32_t word);

/* Encode packed samples into runs. Adjacent equal words are merged.
 *
 * samples        : packed payload, sampleCount * strideBytes bytes
 * outRuns        : destination, capacity maxRuns
 * outRunCount    : number of runs written
 */
int usn_rle_encode(const void* samples, uint32_t sampleCount, uint8_t strideBytes,
                   struct UsnRleRun* outRuns, uint32_t maxRuns, uint32_t* outRunCount);

/* Decode runs into packed samples.
 *
 * outSamples     : destination, capacity maxSamples * strideBytes bytes
 * outSampleCount : number of samples written
 */
int usn_rle_decode(const struct UsnRleRun* runs, uint32_t runCount, uint8_t strideBytes,
                   void* outSamples, uint32_t maxSamples, uint32_t* outSampleCount);

/* Sum of all run counts. Lets the host validate decodedSampleCount BEFORE
 * allocating, so a corrupt count cannot cause a huge allocation. */
uint64_t usn_rle_total_samples(const struct UsnRleRun* runs, uint32_t runCount);

/* Byte size of an encoded body: prefix + runs. */
static inline uint64_t usn_rle_body_bytes(uint32_t runCount)
{
    return (uint64_t)USN_WIRE_RLE_PREFIX_SIZE + (uint64_t)runCount * (uint64_t)sizeof(struct UsnRleRun);
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* USN_WIRE_RLE_H */
