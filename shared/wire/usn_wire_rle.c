/* ==========================================================================
 * usn_wire_rle.c -- reference RLE codec (see usn_wire_rle.h).
 * ========================================================================== */

#include "usn_wire_rle.h"

static int usn_stride_valid(uint8_t strideBytes)
{
    return (strideBytes == 1u || strideBytes == 2u || strideBytes == 4u) ? 1 : 0;
}

uint32_t usn_wire_read_word(const void* samples, uint32_t index, uint8_t strideBytes)
{
    const uint8_t* p;
    if (samples == NULL || !usn_stride_valid(strideBytes)) { return 0u; }
    p = (const uint8_t*)samples + (size_t)index * (size_t)strideBytes;
    switch (strideBytes) {
    case 1: return (uint32_t)p[0];
    case 2: return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
    case 4: return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                               | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    default: return 0u;
    }
}

void usn_wire_write_word(void* samples, uint32_t index, uint8_t strideBytes, uint32_t word)
{
    uint8_t* p;
    if (samples == NULL || !usn_stride_valid(strideBytes)) { return; }
    p = (uint8_t*)samples + (size_t)index * (size_t)strideBytes;
    switch (strideBytes) {
    case 1:
        p[0] = (uint8_t)(word & 0xFFu);
        break;
    case 2:
        p[0] = (uint8_t)(word & 0xFFu);
        p[1] = (uint8_t)((word >> 8) & 0xFFu);
        break;
    case 4:
        p[0] = (uint8_t)(word & 0xFFu);
        p[1] = (uint8_t)((word >> 8) & 0xFFu);
        p[2] = (uint8_t)((word >> 16) & 0xFFu);
        p[3] = (uint8_t)((word >> 24) & 0xFFu);
        break;
    default:
        break;
    }
}

int usn_rle_encode(const void* samples, uint32_t sampleCount, uint8_t strideBytes,
                   struct UsnRleRun* outRuns, uint32_t maxRuns, uint32_t* outRunCount)
{
    uint32_t i;
    uint32_t runs = 0u;
    uint32_t currentWord;
    uint32_t currentCount;

    if (outRunCount == NULL) { return USN_RLE_ERR_ARGUMENT; }
    *outRunCount = 0u;
    if (samples == NULL || outRuns == NULL || sampleCount == 0u ||
        !usn_stride_valid(strideBytes)) {
        return USN_RLE_ERR_ARGUMENT;
    }

    currentWord = usn_wire_read_word(samples, 0u, strideBytes);
    currentCount = 1u;

    for (i = 1u; i < sampleCount; ++i) {
        const uint32_t w = usn_wire_read_word(samples, i, strideBytes);
        if (w == currentWord) {
            ++currentCount;
            continue;
        }
        if (runs >= maxRuns) { return USN_RLE_ERR_OUTPUT_FULL; }
        outRuns[runs].word = currentWord;
        outRuns[runs].count = currentCount;
        ++runs;
        currentWord = w;
        currentCount = 1u;
    }

    if (runs >= maxRuns) { return USN_RLE_ERR_OUTPUT_FULL; }
    outRuns[runs].word = currentWord;
    outRuns[runs].count = currentCount;
    ++runs;

    *outRunCount = runs;
    return USN_RLE_OK;
}

int usn_rle_decode(const struct UsnRleRun* runs, uint32_t runCount, uint8_t strideBytes,
                   void* outSamples, uint32_t maxSamples, uint32_t* outSampleCount)
{
    uint32_t r;
    uint32_t written = 0u;

    if (outSampleCount == NULL) { return USN_RLE_ERR_ARGUMENT; }
    *outSampleCount = 0u;
    if (outSamples == NULL || !usn_stride_valid(strideBytes)) { return USN_RLE_ERR_ARGUMENT; }
    if (runs == NULL && runCount != 0u) { return USN_RLE_ERR_ARGUMENT; }

    for (r = 0u; r < runCount; ++r) {
        const uint32_t count = runs[r].count;
        uint32_t k;
        if (count == 0u) {
            /* A zero-length run is never produced by the encoder, so seeing one
             * means the body is corrupt. Refuse rather than silently skip. */
            return USN_RLE_ERR_COUNT;
        }
        /* Accumulate in 64 bit: a corrupt count must not wrap the bound check. */
        if ((uint64_t)written + (uint64_t)count > (uint64_t)maxSamples) {
            return USN_RLE_ERR_OUTPUT_FULL;
        }
        for (k = 0u; k < count; ++k) {
            usn_wire_write_word(outSamples, written + k, strideBytes, runs[r].word);
        }
        written += count;
    }

    *outSampleCount = written;
    return USN_RLE_OK;
}

uint64_t usn_rle_total_samples(const struct UsnRleRun* runs, uint32_t runCount)
{
    uint64_t total = 0u;
    uint32_t r;
    if (runs == NULL) { return 0u; }
    for (r = 0u; r < runCount; ++r) { total += (uint64_t)runs[r].count; }
    return total;
}
