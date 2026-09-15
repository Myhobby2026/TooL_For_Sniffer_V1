# TooL_For_Sniffer_V1 — Universal Sniffer

A professional universal logic analyzer, digital signal sniffer, protocol analyzer, and bus monitor platform.

**Target stack:** C++20 · Qt 6 / QML · CMake + Ninja · MSVC / GCC / Clang · Teensy 4.1 (NXP i.MX RT1062) · Windows 10/11 64-bit, portable to Linux & macOS.

---

## Project Status

| Phase | Status |
|---|---|
| Architecture Review | ✅ **Complete** |
| Phase 1 — Foundation | ✅ **Complete** (All 15 deliverables P1.1–P1.15 delivered, 100% tests passing) |
| Phase 2 — Hardware Characterisation & Capture Firmware | ⏸ Next |
| Phases 3–18 | ⏸ Planned |

---

## Phase 1 Deliverables Summary

- **Layered Architecture:** Strict downward-only DAG enforced at compile time (`shared/wire` → `common` → `model` → `transport` → `trigger` → `hal` → `protocol` → `core` → `app` → `gui`).
- **Qt-Free Seam (Rule A):** Verified mechanically via `architecture.qt_free_core` CTest; 100% of the core engine and tests build headless without Qt.
- **Single Source of Truth Wire Protocol:** `shared/wire/usn_wire.h` is C-compatible, checked against firmware cross-compilation, and protected by Castagnoli CRC32C and RLE compression.
- **Test Infrastructure:** 21 automated tests spanning unit, integration, golden-file validation, and codec fuzzing.
- **CLI Utilities:** `wire_dumper` (diagnoses raw captures) and `diagnostics` (gathers environment & runtime telemetry into JSON bundles).
- **Desktop Application:** Qt 6 QML bootstrap shell with live Diagnostics and Capture view-models.

---

## Quick Start

### Build Headless Core & Run Tests
```bash
# Configure headless core
cmake --preset core-only

# Build core and utilities
cmake --build --preset core-only

# Run complete test suite
ctest --test-dir build/core-only --output-on-failure
```

### Build with Qt 6 Desktop GUI
```bash
cmake -B build-gui -GNinja -DUSN_ENABLE_GUI=ON
cmake --build build-gui
```

---

## Documentation

- **[docs/architecture.md](docs/architecture.md)** — Architectural design, dependency DAG, and threading model.
- **[docs/capture_protocol.md](docs/capture_protocol.md)** — Wire format specification, packet structures, and worked examples.
- **[docs/file_format.md](docs/file_format.md)** — `.usn` capture container specification.
- **[docs/decoder_api.md](docs/decoder_api.md)** — Protocol decoder API guide and checkpoint design.
- **[docs/trigger_engine.md](docs/trigger_engine.md)** — Trigger AST, combinators, and classification engine.
- **[docs/hardware_constraints.md](docs/hardware_constraints.md)** — Verified Teensy 4.1 hardware limits and measurement record templates.
- **[docs/development.md](docs/development.md)** — Developer guide, CMake presets, and contribution rules.
- **[docs/adr/](docs/adr/)** — Architecture Decision Records (ADRs 0001–0004).

---

## Headline Engineering Constraint

> Sustained capture rate on Teensy 4.1 is bounded by **USB payload throughput**, and burst depth is bounded by **on-chip RAM** (≈512 KB OCRAM; external PSRAM is optional). The GPIO sampler is not the bottleneck. `480 Mbit/s` is a USB *link* rate, not a payload rate — the realistic planning envelope is **5–20 MB/s**, which at 4 bytes per sample (≤32 channels) is **1.25–5 MSPS continuous**.

No sample-rate figure will appear in this project's UI or documentation until it is backed by a committed measurement record on named hardware. See `docs/hardware_constraints.md`.

---

## License

MIT License. See [LICENSE](LICENSE).
