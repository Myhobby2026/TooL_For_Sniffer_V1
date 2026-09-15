# Universal Sniffer — Hardware Constraints & Verification Records

**Target Platform:** PJRC Teensy 4.1 (NXP i.MX RT1062, ARM Cortex-M7 @ 600 MHz)

---

## 1. Verified Hardware Ceilings

The following constraints are verified against vendor datasheets and empirical reports:

| Subsystem | Verified Constraint | Architectural Impact |
|---|---|---|
| **USB Physical Link** | 480 Mbit/s (High Speed link, not payload) | Payload throughput capped at 5–25 MB/s. Never claim 60 MB/s. |
| **USB CDC Stream** | ~10–20 MB/s sustained host transfer | At 4 bytes/sample (<=32 channels), max continuous rate is 2.5–5 MSPS. |
| **Internal RAM** | 1024 KB total (512 KB DTCM/ITCM, 512 KB OCRAM) | Pre-trigger buffer without external RAM is strictly limited to milliseconds at high rates. |
| **TCM DMA Access** | TCM is not accessible via AXI bus DMA | DMA buffers must be placed in OCRAM (`RAM2`) or external PSRAM, never DTCM. |
| **External PSRAM** | QSPI @ 105.6 MHz ≈ 44–60 MB/s raw burst | Optional solder-on PSRAM extends burst depth to seconds, but bandwidth is shared. |
| **I/O Voltage** | 3.3 V CMOS levels (**NOT 5 V tolerant**) | External level shifting/buffering is required for 5V systems. |
| **GPIO Pin Map** | Only a subset of GPIO pins map to DMA-capable ports | Digital channel count is physically linked to DMA port routing. |

---

## 2. Measurement Record Template

Per project specification, no sample-rate or throughput claim may be presented in the user interface or marketing documentation without an accompanying verified measurement record committed below.

```markdown
### Measurement Record: [ID]
- **Date:** YYYY-MM-DD
- **Hardware Revision:** Teensy 4.1 (PCB Rev D)
- **PSRAM Fitted:** [Yes (size) / No]
- **Firmware Commit SHA:** [git sha]
- **Host OS & Toolchain:** Windows 11 64-bit / Linux 6.x
- **Host USB Controller:** xHCI USB 3.0 / 2.0 Host Port
- **Test Duration:** [seconds]
- **Channel Count & Stride:** [Channels, Stride Bytes]
- **Measured Sustained Throughput:** [MB/s]
- **Measured Sustained Sample Rate:** [MSPS]
- **Packet Loss / Overflows:** [0 / count]
- **Sign-off By:** [Name/Role]
```
