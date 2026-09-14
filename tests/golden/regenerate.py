#!/usr/bin/env python3
"""Regenerate tests/golden/wire_sample_block from the specification.

The layout and the CRC32C polynomial are taken from shared/wire/usn_wire.h. This
script deliberately does NOT read the C++ encoder: the point is that two independent
implementations of the same specification must agree, and the C++ golden test is
what compares them.

Run from the repository root. Review `git diff tests/golden` before committing.
"""
import json
import os
import struct

POLY = 0x82F63B78  # reflected form of the Castagnoli polynomial 0x1EDC6F41


def crc32c(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (POLY if (crc & 1) else 0)
    return crc ^ 0xFFFFFFFF


def build_packet(config: dict) -> bytes:
    block = config["block"]
    count = block["sampleCount"]
    stride = block["strideBytes"]
    if block["payloadPattern"] != "counter16" or stride != 2:
        raise SystemExit("this generator only knows the counter16 pattern at stride 2")
    payload = b"".join(struct.pack("<H", i & 0xFFFF) for i in range(count))

    # 40-byte SAMPLE_BLOCK prefix; offsets per shared/wire/usn_wire.h.
    prefix = struct.pack(
        "<QQIHBxQQ",
        block["firstSampleIndex"],
        block["firstDeviceTick"],
        count,
        block["channelCount"],
        stride,
        block["channelMask"],
        block["sampleRateHz"],
    )
    if len(prefix) != 40:
        raise SystemExit(f"prefix must be 40 bytes, got {len(prefix)}")
    body = prefix + payload

    # 32-byte header; headerCrc32c covers bytes [0..27].
    header28 = struct.pack(
        "<IBBBBIIQ",
        0x314E5355,
        config["headerVersion"],
        config["packetType"],
        config["flags"],
        config["headerLength"],
        config["sequence"],
        len(body),
        config["streamId"],
    )
    header28 += struct.pack("<I", crc32c(body))
    if len(header28) != 28:
        raise SystemExit(f"header prefix must be 28 bytes, got {len(header28)}")
    return header28 + struct.pack("<I", crc32c(header28)) + body


def main() -> None:
    if crc32c(b"123456789") != 0xE3069283:
        raise SystemExit("CRC32C self-test failed; refusing to write goldens")

    here = os.path.dirname(os.path.abspath(__file__))
    for case in sorted(os.listdir(here)):
        config_path = os.path.join(here, case, "config.json")
        if not os.path.isfile(config_path):
            continue
        with open(config_path, encoding="utf-8") as handle:
            config = json.load(handle)
        packet = build_packet(config)
        with open(os.path.join(here, case, "input.bin"), "wb") as handle:
            handle.write(packet)
        with open(os.path.join(here, case, "expected.hex"), "w", encoding="utf-8") as handle:
            handle.write(
                "# Golden wire packet, generated from the field layout and CRC32C\n"
                "# polynomial in shared/wire/usn_wire.h by an independent Python\n"
                "# implementation (tests/golden/regenerate.py). One byte per hex\n"
                "# pair; '#' starts a comment.\n"
            )
            for offset in range(0, len(packet), 16):
                chunk = packet[offset : offset + 16]
                handle.write(" ".join(f"{byte:02x}" for byte in chunk) + "\n")
        print(f"{case}: {len(packet)} bytes")


if __name__ == "__main__":
    main()
