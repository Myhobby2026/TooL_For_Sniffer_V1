# Universal Sniffer — Developer Guide

**Project Standard:** C++20  
**Build System:** CMake (>= 3.21) + Ninja  
**Compilers Supported:** GCC (>= 12), Clang (>= 15), MSVC (Visual Studio 2022)

---

## 1. Quick Start

### Build Headless Core & Run Test Suite
```bash
# Configure headless core preset
cmake --preset core-only

# Build core libraries, tools, and tests
cmake --build --preset core-only

# Run complete test suite (unit, integration, fuzz, golden)
ctest --test-dir build/core-only --output-on-failure
```

### Build with Qt 6 Desktop GUI
```bash
# Requires Qt 6.5+ (Qt 6.8 LTS recommended)
cmake -B build-gui -GNinja -DUSN_ENABLE_GUI=ON
cmake --build build-gui
```

---

## 2. CMake Presets

| Preset Name | Target System | Features |
|---|---|---|
| `core-only` | Linux / macOS / Windows | Headless C++20 core, test suite, zero Qt dependency |
| `windows-debug` | Windows / MSVC | Debug build with `/W4 /WX /permissive-` |
| `windows-release`| Windows / MSVC | Optimized Release build |
| `linux-debug` | Linux / GCC | Headless Debug build |
| `linux-release` | Linux / GCC | Headless Release build |
| `asan` | Linux / GCC | AddressSanitizer + UndefinedBehaviorSanitizer |
| `tsan` | Linux / GCC | ThreadSanitizer (capture pipeline and concurrency) |

---

## 3. Test Suites & Labels

Tests are categorized using CTest labels:
- `ctest -L unit`: Fast unit tests (common, model, hal, protocol, trigger, transport).
- `ctest -L integration`: Multi-layer end-to-end tests and CLI integration.
- `ctest -L golden`: Byte-exact golden wire packet validation against Python spec.
- `ctest -L fuzz`: Codec fuzzing with corrupted/truncated input streams.
- `ctest -L architecture`: Mechanical enforcement of Rule A (Qt-free core) and single-source-of-truth wire headers.

---

## 4. Phase 1 Exit Gate Checklist

All criteria must be satisfied prior to beginning Phase 2:
- [x] `cmake --preset core-only && cmake --build --preset core-only && ctest` green, zero warnings
- [x] `windows-debug` and `windows-release` presets configured and verified
- [x] Qt 6 GUI preset defined and offscreen build supported
- [x] Transport codec fuzz passing over randomized inputs
- [x] Architecture layer test green (`architecture.qt_free_core` and `architecture.wire_header_shared`)
- [x] `wire_format_test` green verifying binary sizes, offsets, CRC, and RLE
- [x] All public interfaces documented in `docs/`
- [x] ADRs 0001–0004 recorded and accepted
- [x] `docs/hardware_constraints.md` includes measurement record templates
