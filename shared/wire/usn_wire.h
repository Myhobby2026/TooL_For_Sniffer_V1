/* ==========================================================================
 * usn_wire.h -- Universal Sniffer USB wire format.
 *
 * SINGLE SOURCE OF TRUTH. This header is included by BOTH the desktop
 * transport layer and the Teensy 4.1 firmware. It is deliberately
 * C-compatible (no C++-only constructs) so arm-none-eabi-gcc can compile the
 * identical file into the firmware image. A layout change that breaks one side
 * breaks the other's static assertion at compile time -- which is the entire
 * point (docs/architecture_review.md section 8.2, risk R4/R20).
 *
 * Byte order: ALL multi-byte fields are little-endian, on the wire and in
 * these structures. Both supported hosts (x86-64 Windows, x86-64/ARM Linux,
 * ARM macOS) and the Cortex-M7 are little-endian, so the structures may be
 * memcpy'd; the desktop codec still byte-swaps explicitly when
 * USN_WIRE_ASSUME_NATIVE_ENDIAN is not defined, so a big-endian host works.
 *
 * Packing: every structure is packed to 1 byte. Offsets are asserted below.
 * ========================================================================== */

#ifndef USN_WIRE_H
#define USN_WIRE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
#  define USN_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#  define USN_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

#if defined(_MSC_VER) || defined(__GNUC__) || defined(__clang__)
#  define USN_WIRE_PACK_BEGIN _Pragma("pack(push, 1)")
#  define USN_WIRE_PACK_END   _Pragma("pack(pop)")
#else
#  error "usn_wire.h requires a compiler supporting #pragma pack"
#endif

/* --------------------------------------------------------------------------
 * Protocol constants
 * -------------------------------------------------------------------------- */

/* Magic bytes on the wire are 'U','S','N','1' = 0x55 0x53 0x4E 0x31.
 * Read as a little-endian u32 that is 0x314E5355. */
#define USN_WIRE_MAGIC_VALUE      0x314E5355u
#define USN_WIRE_MAGIC_BYTE_0     0x55u  /* 'U' */
#define USN_WIRE_MAGIC_BYTE_1     0x53u  /* 'S' */
#define USN_WIRE_MAGIC_BYTE_2     0x4Eu  /* 'N' */
#define USN_WIRE_MAGIC_BYTE_3     0x31u  /* '1' */

#define USN_WIRE_VERSION              1u
#define USN_WIRE_MIN_SUPPORTED_VERSION 1u
#define USN_WIRE_HEADER_SIZE         32u

/* Hard cap on bodyLength. A corrupt length field must never be able to make
 * the host allocate unboundedly; the parser rejects anything larger and
 * resynchronises on the magic. Bounds total parser memory to 65536 + 32. */
#define USN_WIRE_MAX_BODY_LENGTH  65536u

#define USN_WIRE_SIZE_PREFIX       40u  /* UsnSampleBlockPrefix */
#define USN_WIRE_RLE_PREFIX_SIZE   48u  /* UsnRleBlockPrefix    */

/* --------------------------------------------------------------------------
 * Packet types
 * -------------------------------------------------------------------------- */

enum UsnPacketType {
    USN_PKT_INVALID            = 0x00, /* illegal; resync sentinel in tests */
    USN_PKT_DEVICE_HELLO       = 0x01, /* device to host, at boot and after reset */
    USN_PKT_CAPABILITIES       = 0x02, /* device to host, answers GET_CAPABILITIES */
    USN_PKT_COMMAND            = 0x03, /* host to device */
    USN_PKT_COMMAND_ACK        = 0x04, /* device to host */
    USN_PKT_CAPTURE_START_ACK  = 0x05, /* device to host */
    USN_PKT_CAPTURE_STOP_ACK   = 0x06, /* device to host */
    USN_PKT_SAMPLE_BLOCK       = 0x07, /* device to host -- main data path */
    USN_PKT_SAMPLE_BLOCK_RLE   = 0x08, /* device to host -- compressed data path */
    USN_PKT_TRIGGER_EVENT      = 0x09, /* device to host */
    USN_PKT_DIAGNOSTIC_EVENT   = 0x0A, /* device to host */
    USN_PKT_HEARTBEAT          = 0x0B, /* device to host */
    USN_PKT_TIME_SYNC          = 0x0C, /* device to host */
    USN_PKT_PING               = 0x0D, /* either direction */
    USN_PKT_PONG               = 0x0E, /* either direction */
    USN_PKT_ERROR              = 0x0F, /* device to host */
    USN_PKT_STREAM_END         = 0x10  /* device to host */
};

/* --------------------------------------------------------------------------
 * Header flag bits
 * -------------------------------------------------------------------------- */

enum UsnPacketFlag {
    USN_FLAG_COMPRESSED_RLE       = 0x01,
    USN_FLAG_FIRST_OF_STREAM      = 0x02,
    USN_FLAG_LAST_OF_STREAM       = 0x04,
    /* Samples were lost on the device before this packet. Never silent. */
    USN_FLAG_OVERFLOW_BEFORE      = 0x08,
    USN_FLAG_CRC_PRESENT          = 0x10,
    /* The device restarted since the previous packet (new boot nonce). */
    USN_FLAG_DEVICE_RESET         = 0x20,
    USN_FLAG_TRIGGER_FIRED        = 0x40,
    /* Must be zero. A non-zero reserved bit means the parser is out of sync
     * or the firmware is newer than the host; reject and resynchronise. */
    USN_FLAG_RESERVED             = 0x80
};

/* --------------------------------------------------------------------------
 * Command identifiers (host to device, USN_PKT_COMMAND)
 * -------------------------------------------------------------------------- */

enum UsnCommandId {
    USN_CMD_GET_CAPABILITIES      = 0x0001,
    USN_CMD_SET_CAPTURE_CONFIG    = 0x0002,
    USN_CMD_ARM_TRIGGER           = 0x0003,
    USN_CMD_DISARM_TRIGGER        = 0x0004,
    USN_CMD_START_CAPTURE         = 0x0005,
    USN_CMD_STOP_CAPTURE          = 0x0006,
    USN_CMD_ABORT_CAPTURE         = 0x0007,
    USN_CMD_SELF_TEST             = 0x0008,
    USN_CMD_GENERATE_TEST_PATTERN = 0x0009,
    USN_CMD_GET_COUNTERS          = 0x000A,
    USN_CMD_CLEAR_ERROR           = 0x000B,
    USN_CMD_SET_LED               = 0x000C,
    USN_CMD_RESET_DEVICE          = 0x000D,
    USN_CMD_ENTER_BOOTLOADER      = 0x000E
};

/* COMMAND_ACK status field. Distinct from the desktop ErrorCode enum: these
 * are the small set of outcomes the firmware can report over the wire. */
enum UsnAckStatus {
    USN_ACK_OK                  = 0x00,
    USN_ACK_UNKNOWN_COMMAND     = 0x01,
    USN_ACK_BAD_ARGUMENTS       = 0x02,
    USN_ACK_UNSUPPORTED         = 0x03, /* not in device capabilities */
    USN_ACK_INVALID_STATE       = 0x04,
    USN_ACK_RESOURCE_EXHAUSTED  = 0x05,
    USN_ACK_HARDWARE_ERROR      = 0x06,
    USN_ACK_BUSY                = 0x07
};

/* Diagnostic codes the device can raise (USN_PKT_DIAGNOSTIC_EVENT). */
enum UsnDeviceDiagnostic {
    USN_DIAG_NONE                 = 0x0000,
    USN_DIAG_DMA_OVERFLOW         = 0x0001,
    USN_DIAG_RING_OVERFLOW        = 0x0002,
    USN_DIAG_USB_BACKPRESSURE     = 0x0003,
    USN_DIAG_SAMPLE_CLOCK_UNSTABLE= 0x0004,
    USN_DIAG_TRIGGER_MISSED       = 0x0005,
    USN_DIAG_WATCHDOG_RESET       = 0x0006,
    USN_DIAG_BUFFER_UNDERRUN      = 0x0007,
    USN_DIAG_CONFIG_REJECTED      = 0x0008,
    USN_DIAG_SELF_TEST_FAILED     = 0x0009,
    USN_DIAG_THERMAL_THROTTLE     = 0x000A
};

USN_WIRE_PACK_BEGIN

/* --------------------------------------------------------------------------
 * Packet header -- exactly 32 bytes, present on every packet.
 *
 * Two CRCs, not one: a corrupt header must be detectable BEFORE bodyLength is
 * trusted, otherwise one bad byte can desynchronise the stream for the rest of
 * the capture or trigger a huge allocation.
 * -------------------------------------------------------------------------- */

struct UsnPacketHeader {
    uint32_t magic;          /* @0  USN_WIRE_MAGIC_VALUE                        */
    uint8_t  headerVersion;  /* @4  USN_WIRE_VERSION                            */
    uint8_t  packetType;     /* @5  enum UsnPacketType                          */
    uint8_t  flags;          /* @6  enum UsnPacketFlag, bitwise or              */
    uint8_t  headerLength;   /* @7  >= 32; parser skips the excess, so the
                                   header can grow without breaking old hosts   */
    uint32_t sequence;       /* @8  per stream, +1 per packet, wraps            */
    uint32_t bodyLength;     /* @12 <= USN_WIRE_MAX_BODY_LENGTH                 */
    uint64_t streamId;       /* @16 boot nonce ^ counter; detects device reset  */
    uint32_t bodyCrc32c;     /* @24 CRC32C over the body; 0 if !CRC_PRESENT     */
    uint32_t headerCrc32c;   /* @28 CRC32C over header bytes [0..27]            */
};

USN_STATIC_ASSERT(sizeof(struct UsnPacketHeader) == USN_WIRE_HEADER_SIZE,
                  "UsnPacketHeader must be exactly 32 bytes");
USN_STATIC_ASSERT(offsetof(struct UsnPacketHeader, magic)         ==  0, "magic offset");
USN_STATIC_ASSERT(offsetof(struct UsnPacketHeader, headerVersion) ==  4, "headerVersion offset");
USN_STATIC_ASSERT(offsetof(struct UsnPacketHeader, packetType)    ==  5, "packetType offset");
USN_STATIC_ASSERT(offsetof(struct UsnPacketHeader, flags)         ==  6, "flags offset");
USN_STATIC_ASSERT(offsetof(struct UsnPacketHeader, headerLength)  ==  7, "headerLength offset");
USN_STATIC_ASSERT(offsetof(struct UsnPacketHeader, sequence)      ==  8, "sequence offset");
USN_STATIC_ASSERT(offsetof(struct UsnPacketHeader, bodyLength)    == 12, "bodyLength offset");
USN_STATIC_ASSERT(offsetof(struct UsnPacketHeader, streamId)      == 16, "streamId offset");
USN_STATIC_ASSERT(offsetof(struct UsnPacketHeader, bodyCrc32c)    == 24, "bodyCrc32c offset");
USN_STATIC_ASSERT(offsetof(struct UsnPacketHeader, headerCrc32c)  == 28, "headerCrc32c offset");

/* --------------------------------------------------------------------------
 * USN_PKT_SAMPLE_BLOCK body: 40-byte prefix then tightly packed payload.
 *
 * payload is sampleCount * strideBytes bytes. strideBytes is 1 for up to 8
 * channels, 2 for up to 16, 4 for up to 32 -- samples stay packed end to end
 * so an 8-channel capture costs a quarter of the RAM, disk and cache of a
 * widened representation (deviation D2).
 * -------------------------------------------------------------------------- */

struct UsnSampleBlockPrefix {
    uint64_t firstSampleIndex; /* @0  authoritative position key               */
    uint64_t firstDeviceTick;  /* @8                                          */
    uint32_t sampleCount;      /* @16 > 0                                     */
    uint16_t channelCount;     /* @20 1..64                                   */
    uint8_t  strideBytes;      /* @22 1, 2 or 4                               */
    uint8_t  reserved;         /* @23 must be 0                               */
    uint64_t channelMask;      /* @24 which physical bits are meaningful      */
    uint64_t sampleRateHz;     /* @32 repeated per block: a rate change is
                                     self-describing and the timeline stays
                                     exactly reconstructible                 */
};

USN_STATIC_ASSERT(sizeof(struct UsnSampleBlockPrefix) == USN_WIRE_SIZE_PREFIX,
                  "UsnSampleBlockPrefix must be exactly 40 bytes");
USN_STATIC_ASSERT(offsetof(struct UsnSampleBlockPrefix, firstSampleIndex) ==  0, "prefix offset");
USN_STATIC_ASSERT(offsetof(struct UsnSampleBlockPrefix, firstDeviceTick)  ==  8, "prefix offset");
USN_STATIC_ASSERT(offsetof(struct UsnSampleBlockPrefix, sampleCount)      == 16, "prefix offset");
USN_STATIC_ASSERT(offsetof(struct UsnSampleBlockPrefix, channelCount)     == 20, "prefix offset");
USN_STATIC_ASSERT(offsetof(struct UsnSampleBlockPrefix, strideBytes)      == 22, "prefix offset");
USN_STATIC_ASSERT(offsetof(struct UsnSampleBlockPrefix, reserved)         == 23, "prefix offset");
USN_STATIC_ASSERT(offsetof(struct UsnSampleBlockPrefix, channelMask)      == 24, "prefix offset");
USN_STATIC_ASSERT(offsetof(struct UsnSampleBlockPrefix, sampleRateHz)     == 32, "prefix offset");

/* --------------------------------------------------------------------------
 * USN_PKT_SAMPLE_BLOCK_RLE body: 48-byte prefix then runs.
 *
 * Protocol captures are mostly idle, so run-length encoding typically buys one
 * to three orders of magnitude in capture duration for the same USB bandwidth
 * -- the highest-leverage mitigation for the USB ceiling. decodedSampleCount
 * lets the host validate the expansion BEFORE allocating.
 * -------------------------------------------------------------------------- */

struct UsnRleBlockPrefix {
    struct UsnSampleBlockPrefix block; /* @0  40 bytes                        */
    uint32_t decodedSampleCount;       /* @40 samples represented after
                                              expansion; must equal the sum of
                                              all run counts                 */
    uint32_t runCount;                 /* @44 number of UsnRleRun entries    */
};

struct UsnRleRun {
    uint32_t word;   /* the sample word, zero-extended from strideBytes */
    uint32_t count;  /* 1 .. 0xFFFFFFFF                                 */
};

USN_STATIC_ASSERT(sizeof(struct UsnRleBlockPrefix) == USN_WIRE_RLE_PREFIX_SIZE,
                  "UsnRleBlockPrefix must be exactly 48 bytes");
USN_STATIC_ASSERT(sizeof(struct UsnRleRun) == 8, "UsnRleRun must be exactly 8 bytes");
USN_STATIC_ASSERT(offsetof(struct UsnRleBlockPrefix, decodedSampleCount) == 40, "rle prefix offset");
USN_STATIC_ASSERT(offsetof(struct UsnRleBlockPrefix, runCount)           == 44, "rle prefix offset");

/* --------------------------------------------------------------------------
 * USN_PKT_COMMAND body (host to device)
 * -------------------------------------------------------------------------- */

struct UsnCommandBody {
    uint16_t commandId;  /* @0 enum UsnCommandId */
    uint16_t argLength;  /* @2 bytes of args following this prefix */
};

USN_STATIC_ASSERT(sizeof(struct UsnCommandBody) == 4, "UsnCommandBody must be 4 bytes");

/* --------------------------------------------------------------------------
 * USN_PKT_COMMAND_ACK body (device to host)
 * -------------------------------------------------------------------------- */

struct UsnCommandAckBody {
    uint16_t commandId;  /* @0 the command being answered */
    uint8_t  status;     /* @2 enum UsnAckStatus */
    uint8_t  reserved;   /* @3 must be 0 */
    uint16_t detailLength; /* @4 bytes of detail following this prefix */
};

USN_STATIC_ASSERT(sizeof(struct UsnCommandAckBody) == 6, "UsnCommandAckBody must be 6 bytes");

/* --------------------------------------------------------------------------
 * USN_PKT_DEVICE_HELLO body
 * -------------------------------------------------------------------------- */

struct UsnDeviceHelloBody {
    uint64_t bootNonce;      /* @0  random per boot; streamId is derived from it,
                                    which is how a mid-capture device reset is
                                    detected rather than inferred            */
    uint32_t firmwareVersion;/* @8  major<<24 | minor<<16 | patch<<8 | build */
    uint32_t hardwareRevision; /* @12 */
    uint32_t wireVersion;    /* @16 USN_WIRE_VERSION the device speaks */
    uint32_t uptimeMs;       /* @20 */
};

USN_STATIC_ASSERT(sizeof(struct UsnDeviceHelloBody) == 24,
                  "UsnDeviceHelloBody must be 24 bytes");

/* --------------------------------------------------------------------------
 * USN_PKT_TRIGGER_EVENT body
 * -------------------------------------------------------------------------- */

struct UsnTriggerEventBody {
    uint64_t streamId;       /* @0  */
    uint64_t sampleIndex;    /* @8  exact trigger position -- every view aligns
                                    to this same integer, so cursor-at-trigger
                                    is exactly reproducible in a golden test */
    uint64_t deviceTick;     /* @16 */
    uint16_t triggerId;      /* @24 */
    uint16_t reasonCode;     /* @26 */
};

USN_STATIC_ASSERT(sizeof(struct UsnTriggerEventBody) == 28,
                  "UsnTriggerEventBody must be 28 bytes");

/* --------------------------------------------------------------------------
 * USN_PKT_HEARTBEAT body
 * -------------------------------------------------------------------------- */

struct UsnHeartbeatBody {
    uint64_t deviceTick;     /* @0  */
    uint64_t sampleIndex;    /* @8  */
    uint32_t uptimeMs;       /* @16 */
    uint32_t txQueueDepth;   /* @20 bytes awaiting USB transmission */
    uint32_t overflowTotal;  /* @24 cumulative samples lost on the device */
    uint32_t reserved;       /* @28 must be 0 */
};

USN_STATIC_ASSERT(sizeof(struct UsnHeartbeatBody) == 32,
                  "UsnHeartbeatBody must be 32 bytes");

USN_WIRE_PACK_END

/* --------------------------------------------------------------------------
 * Stride selection: the narrowest word that holds channelCount bits.
 * -------------------------------------------------------------------------- */

static inline uint8_t usn_wire_stride_for_channels(uint16_t channelCount)
{
    if (channelCount == 0u)  { return 0u; }
    if (channelCount <= 8u)  { return 1u; }
    if (channelCount <= 16u) { return 2u; }
    if (channelCount <= 32u) { return 4u; }
    return 0u; /* >32 channels is not representable in wire version 1 */
}

/* Payload byte count for a sample block, or 0 when the combination is invalid.
 * The multiplication is done in 64 bit so a hostile sampleCount cannot wrap. */
static inline uint64_t usn_wire_sample_payload_bytes(uint32_t sampleCount, uint8_t strideBytes)
{
    if (strideBytes != 1u && strideBytes != 2u && strideBytes != 4u) { return 0u; }
    if (sampleCount == 0u) { return 0u; }
    return (uint64_t)sampleCount * (uint64_t)strideBytes;
}

#endif /* USN_WIRE_H */
