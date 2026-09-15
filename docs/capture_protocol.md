# Universal Sniffer — USB Wire Protocol Specification

**Status:** Frozen (Phase 1 Baseline)  
**Header Definition:** `shared/wire/usn_wire.h`  
**CRC Definition:** `shared/wire/usn_wire_crc.h`  
**RLE Definition:** `shared/wire/usn_wire_rle.h`

---

## 1. Design Principles

1. **Shared Single Source of Truth:** `shared/wire/usn_wire.h` is written in standard C99/C11 and is compiled by both the Teensy 4.1 firmware (`arm-none-eabi-gcc`) and the host application.
2. **Byte Order:** Little-endian across all fields.
3. **Alignment & Packing:** Explicit 1-byte packing with static compile-time assertions on all sizes and member offsets.
4. **Resynchronisation & Dual CRC:** Every packet contains a 32-byte header with magic bytes `0x314E5355` ("USN1"), a dedicated header CRC32C, and a body CRC32C. If data corruption occurs, the parser resynchronizes on the magic word without unbounded memory allocation.

---

## 2. Packet Header Layout (32 Bytes)

```text
+-------------------+-------------------+-------------------+-------------------+
|      Offset       |       Type        |       Field       |    Description    |
+-------------------+-------------------+-------------------+-------------------+
| 0x00 .. 0x03      | uint32_t          | magic             | 0x314E5355 (USN1) |
| 0x04              | uint8_t           | headerVersion     | 0x01              |
| 0x05              | uint8_t           | packetType        | UsnPacketType     |
| 0x06              | uint8_t           | flags             | UsnPacketFlag     |
| 0x07              | uint8_t           | headerLength      | 32 (>= 32)        |
| 0x08 .. 0x0B      | uint32_t          | sequence          | Wraparound seq no |
| 0x0C .. 0x0F      | uint32_t          | bodyLength        | Length of payload |
| 0x10 .. 0x17      | uint64_t          | streamId          | Reset/session ID  |
| 0x18 .. 0x1B      | uint32_t          | bodyCrc32c        | Castagnoli CRC32C |
| 0x1C .. 0x1F      | uint32_t          | headerCrc32c      | CRC of [0..27]    |
+-------------------+-------------------+-------------------+-------------------+
```

---

## 3. Packet Types

| Type ID | Enum Symbol | Direction | Purpose |
|---|---|---|---|
| 0x01 | `USN_PKT_DEVICE_HELLO` | Device -> Host | Announce presence and hardware nonce at boot |
| 0x02 | `USN_PKT_CAPABILITIES` | Device -> Host | Report supported sample rates and channels |
| 0x03 | `USN_PKT_COMMAND` | Host -> Device | Arm, start, stop, configure capture |
| 0x04 | `USN_PKT_COMMAND_ACK` | Device -> Host | Command acknowledgment or error |
| 0x07 | `USN_PKT_SAMPLE_BLOCK` | Device -> Host | Uncompressed digital sample stream |
| 0x08 | `USN_PKT_SAMPLE_BLOCK_RLE` | Device -> Host | Run-length encoded sample stream |
| 0x09 | `USN_PKT_TRIGGER_EVENT` | Device -> Host | Trigger match timestamp and sample index |
| 0x0A | `USN_PKT_DIAGNOSTIC_EVENT` | Device -> Host | Hardware overflow, FIFO warning, error |
| 0x0B | `USN_PKT_HEARTBEAT` | Device -> Host | Link keepalive and health counters |

---

## 4. Sample Block Layout (`USN_PKT_SAMPLE_BLOCK`)

A sample block contains a 40-byte prefix followed by packed digital sample words:
- Stride 1: Channels <= 8 (1 byte per sample)
- Stride 2: Channels <= 16 (2 bytes per sample, little-endian)
- Stride 4: Channels <= 32 (4 bytes per sample, little-endian)

Prefix fields:
- `firstSampleIndex` (uint64_t): Absolute 0-based sample index of the first sample.
- `firstTick` (uint64_t): Hardware clock timestamp.
- `sampleCount` (uint32_t): Number of samples in this block.
- `channelCount` (uint16_t): Number of active digital channels.
- `strideBytes` (uint8_t): 1, 2, or 4.
- `flags` (uint8_t): Overflow indicators, reset detection, trigger status.
- `channelMask` (uint64_t): Bitmask of active channels.
- `sampleRateHz` (uint64_t): Sampling clock frequency.
