# Universal Sniffer — File Format Specification (`.usn`)

**Status:** Current (Phase 1 Baseline)  
**File Extension:** `.usn`  
**Container Type:** Binary chunked stream with table of contents

---

## 1. Overview & Structure

The `.usn` file format stores long-duration digital signal captures and protocol decoded events. It is designed for:
1. **Crash-Resilience:** Written sequentially in chunks; a truncated file can be repaired using recovered chunk boundaries.
2. **Larger-Than-RAM Seeking:** Sample blocks are grouped into fixed-size indexable chunks; views can be memory-mapped or streamed on demand.
3. **Embedded Metadata:** Stores device capabilities, channel configurations, trigger definitions, and diagnostic audit logs.

---

## 2. File Layout

```text
+-------------------------------------------------------+
| File Header (32 bytes, Magic: 'USN\x02', version)     |
+-------------------------------------------------------+
| Metadata Section (JSON: config, channel names, colors)|
+-------------------------------------------------------+
| Chunk 0: Sample Blocks (Compressed or Raw)            |
+-------------------------------------------------------+
| Chunk 1: Sample Blocks                                |
+-------------------------------------------------------+
| ...                                                   |
+-------------------------------------------------------+
| Decoded Event Records                                 |
+-------------------------------------------------------+
| Diagnostics Log Records                               |
+-------------------------------------------------------+
| Chunk Index Table (Offsets and Sample Ranges)         |
+-------------------------------------------------------+
| File Footer (TOC offset, Checksum)                    |
+-------------------------------------------------------+
```
