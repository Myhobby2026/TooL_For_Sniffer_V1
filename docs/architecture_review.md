# Universal Sniffer — Architecture Review

**Status:** Proposed — awaiting approval
**Date:** 2026-09-14
**Author:** Engineering agent (Arena.ai Agent Mode)
**Repository state at time of writing:** greenfield (`README.md` only, commit `4dae474`)
**Scope of this document:** architecture review per master specification §53. **No production code has been written.** Phase 1 implementation begins only after this review is accepted.

---

## 0. Executive summary

This review proposes a layered, Qt-free-core architecture in which the capture engine, protocol
decoders, transport, and file format are plain C++20 libraries with **zero Qt dependency**, and Qt 6
appears only in the top two layers (`app`, `gui`). This is a deliberate strengthening of the layering
in master spec §4: it makes ~80 % of the product unit-testable headless in CI, on any compiler,
without a display server.

The single most important finding of this review is **not** software. It is a hardware/transport
ceiling:

> On Teensy 4.1, sustained capture rate is bounded by **USB payload throughput**, and burst capture
> depth is bounded by **on-chip RAM bandwidth and capacity**. The GPIO sampler is *not* the
> bottleneck. Any sample-rate figure quoted without stating channel count, transport, and capture
> duration is meaningless.

Verified constraints (sources in §16):

| Constraint | Verified value | Consequence for architecture |
|---|---|---|
| USB link rate | 480 Mbit/s (link, **not** payload) | Do not quote 60 MB/s. Plan for 5–20 MB/s **measured**. |
| Practical Teensy CDC throughput | ~2.5–25 MB/s reported, host-software dependent | Transport throughput must be *measured in Phase 3* before any rate is promised. |
| GPIO capture via DMA | DMA-accessible GPIO path is **slower** than CPU fast-GPIO path; one reported externally-clocked experiment was reliable ~10 MHz with misses above | Max sample rate is a **Phase 2 measured spike output**, not a spec input. |
| DMA-accessible GPIO modules | Only a subset of GPIO modules are DMA-reachable | **Channel count and max sample rate are coupled through pin selection.** Pin map is a deliverable. |
| DMA access to TCM | i.MX RT10xx TCM is not on the AXI bus | Capture buffers **must** live in OCRAM (RAM2) or PSRAM, never DTCM/ITCM. |
| PSRAM bandwidth | QSPI ~105.6 MHz default → ~44 MB/s raw burst; ~60 MB/s at 132 MHz | Buffered sample rate limited to ~11–15 MSPS at 4 B/sample; PSRAM is optional solder-on hardware. |
| On-chip RAM | 1024 KB (512 KB TCM), 8 MB flash | Without PSRAM, burst depth at 10 MSPS/32 ch is **milliseconds**. |
| I/O levels | 3.3 V, **not** 5 V tolerant, no isolation, no adjustable threshold | External protection/buffering mandatory for real bus work. Must be documented, not hidden. |
| CAN/LIN/RS485 | Controllers on-chip (3× CAN, 1 CAN-FD), **PHY external** | Model MCU → transceiver → bus explicitly (master spec §22). GPIO is never the physical interface. |

Three architectural consequences follow, and they shape everything below:

1. **Device-side run-length encoding is a first-class feature, not an optimization.** Protocol captures
   are mostly idle. RLE typically buys one to three orders of magnitude in effective capture duration
   for the same USB bandwidth. It belongs in the wire protocol from the start (§8), even though it is
   *implemented* later (§17, measurement-gated).
2. **The wire format and the `.usn` file format must be single-source-of-truth artifacts** shared
   between firmware and desktop (§8, §13). Firmware/host protocol drift is the classic failure mode of
   projects like this.
3. **`SampleIndex` (an integer) — not time — is the primary key** for every record in the system
   (§9, §10). Time is derived, exactly, as a rational quantity. This is what makes gap detection,
   crash recovery, larger-than-RAM seeking, and deterministic golden tests possible.

**Recommended deviations from the master spec** (full §50-format justification in §19):

| # | Deviation | Why |
|---|---|---|
| D1 | Insert a `model` layer of pure value types below `core`/`protocol` | Removes the core↔protocol dependency ambiguity in §4's diagram; decoders stop depending on session/storage types. |
| D2 | `IProtocolDecoder::process(const SampleWindow&)` instead of `std::span<const DigitalSample>` | Packed 1/2/4-byte samples must stay packed for multi-GB captures (§42). A `SampleWindow` accessor gives decoders a stride-aware view; a `span<uint32_t>` adapter is provided for simple decoders. |
| D3 | Core transport implements serial I/O directly (Win32 overlapped / POSIX termios), **not** `QSerialPort` | `QSerialPort` would make the core Qt-dependent and untestable headless. Costs ~300 lines of platform code. |
| D4 | C++ floor is **C++20**, not C++23 | `std::expected` needs GCC 13 / MSVC 19.36; C++23 support is uneven across the CI matrix. C++23 features are opt-in per-target later. |
| D5 | Firmware performs **no** protocol decoding | Two decoder implementations = two sources of truth for protocol semantics. Firmware does capture + framing + RLE + trigger only. |
| D6 | Vendor `{fmt}`, `nlohmann/json`, GoogleTest, Google Benchmark via `FetchContent` | Keeps the core Qt-free and CI-portable. All are permissively licensed (BSL/MIT/Apache-2.0/BSD). |

### 0.1 What was actually verified while writing this review

Per master spec §49 ("never claim a test passed if it was not actually run"), the epistemic status of
each claim class is stated explicitly:

| Claim class | Status | How |
|---|---|---|
| Teensy 4.1 electrical/peripheral specification (§16.1) | **Verified against vendor documentation** retrieved 2026-09-14 (PJRC / Adafruit / SparkFun / DigiKey product data) | web research, values tabulated |
| GPIO-via-DMA is slower than CPU fast-GPIO; ~10 MHz reported reliable in one externally-clocked experiment; only some GPIO modules are DMA-reachable | **Verified against a published technical article** (SparkFun, "Understanding GPIO DMA on Teensy 4.1") and community reports | web research; still marked **[M]** because it must be re-measured on *our* fixture |
| DMA cannot reach TCM on i.MX RT10xx | **Consistent with vendor memory-architecture documentation**; not yet empirically confirmed by us | Phase 2 spike step 6 |
| Practical Teensy USB CDC throughput range (§16.2 L5) | **Verified as a range across multiple independent community reports**; the specific value for our host/firmware is unknown | Phase 2 spike step 4 |
| PSRAM QSPI bandwidth ≈44–60 MB/s | **Verified against published Teensy 4.1 memory documentation** | Phase 2 spike |
| All arithmetic in §7.4, §16.2 (burst depth, IRQ rate, rate ceilings, drift, bandwidth→rate) | **Computed and cross-checked**, not estimated | recomputed programmatically while writing; figures in-document are the computed ones |
| CMake ≥ 4.0 rejects nlohmann/json v3.11.3 (`cmake_minimum_required(VERSION 3.1...3.14)`) | **Verified by fetching and inspecting the actual upstream sources** of all four candidate dependencies | R19, D6 |
| Toolchain availability in the review sandbox | **Verified by execution**: GCC 12.2, GNU Make, CMake 4.4.3 and Ninja 1.13.2 present (CMake/Ninja installed via PyPI); **Qt 6, MSVC, `arm-none-eabi-gcc`, and Teensy hardware all absent**, and no OS package repository is reachable | §18.3 verification matrix |
| Everything else — all interface designs, thread model, file format, decoder API, trigger AST, phase plan | **Proposed design, not verified.** Nothing has been compiled, run, or measured. | becomes verified during Phase 1 |

No code has been written. No test has been run. No sample rate has been measured. The numbers marked
**[M]** in this document are placeholders for measurements that do not yet exist, and master spec §41
forbids presenting them as capabilities.

---

## 1. Recommended final architecture

### 1.1 Dependency DAG

Dependencies point **downward only**. Every arrow is enforced at build time by CMake target
linkage (`target_link_libraries` with `PRIVATE`/`PUBLIC` chosen to prevent leakage), and a
checked-in CMake test (`tests/unit/architecture_layers_test.cpp`) asserts the invariant by parsing the
generated build graph. Reverse edges fail the build.

```text
                        ┌──────────────────────────────┐
                        │  usn::gui   (QML, resources) │   Qt6::Quick
                        └───────────────┬──────────────┘
                                        │
                        ┌───────────────▼──────────────┐
                        │  usn::app  (view-models,     │   Qt6::Core/Gui/Qml
                        │  QAbstractListModels, cmds)  │
                        └───────────────┬──────────────┘
                                        │  Qt-free boundary  ◄── THE critical seam
        ┌───────────────────────────────▼───────────────────────────────┐
        │  usn::core                                                    │
        │  CapturePipeline · CaptureSession · Timeline · StorageEngine  │
        │  ReplaySource · AnalysisEngine · MeasurementEngine            │
        │  SearchEngine · StatisticsEngine · DiagnosticsLog             │
        └──┬──────────────────┬───────────────────────┬─────────────────┘
           │                  │                       │
┌──────────▼─────────┐  ┌─────▼──────────────┐  ┌─────▼──────────────────┐
│ usn::protocol      │  │ usn::hal           │  │ usn::trigger           │
│ IProtocolDecoder   │  │ IDevice            │  │ TriggerNode (AST)      │
│ DecoderRegistry    │  │ ICaptureDevice     │  │ TriggerCompiler        │
│ spi · i2c · uart   │  │ ITriggerEngine     │  │  → device subset       │
│ can · lin · gpio   │  │ DeviceManager      │  │  → host evaluator      │
└──────────┬─────────┘  └─────┬──────────────┘  └─────┬──────────────────┘
           │                  │                       │
           │           ┌──────▼──────────────┐        │
           │           │ usn::transport      │        │
           │           │ ITransport          │        │
           │           │ PacketCodec         │◄───────┘ (trigger cmds ride the transport)
           │           │ UsbCdcTransport     │
           │           │ TransportManager    │
           │           └──────┬──────────────┘
           │                  │
        ┌──▼──────────────────▼───┐
        │  usn::model             │   pure value types, no I/O, no Qt
        │  SampleBlock · Timeline │
        │  types · DecodedEvent   │
        │  DeviceCapabilities     │
        └──────────┬──────────────┘
                   │
        ┌──────────▼──────────────┐
        │  usn::common            │   Status · ErrorCode · log · CRC32C
        │                         │   SpscQueue · RingBuffer · PerfProbe
        └─────────────────────────┘

        ┌─────────────────────────────────────────────┐
        │  shared/wire/usn_wire.h   (C-compatible)    │  ← included by BOTH
        │  packet header layout · packet types ·      │     usn::transport AND
        │  field offsets · static_asserts             │     the Teensy firmware
        └─────────────────────────────────────────────┘

        ┌─────────────────────────────────────────────┐
        │  firmware/teensy41  (separate toolchain)    │  Teensyduino / arm-none-eabi
        └─────────────────────────────────────────────┘
```

### 1.2 The two rules that keep this honest

**Rule A — The Qt-free seam.** `usn::app` is the only layer permitted to include a Qt header below
`gui`. Nothing in `common`, `model`, `transport`, `protocol`, `trigger`, `hal`, or `core` may
`#include <Q...>`. Enforced by a grep-based CMake test and by simply not linking Qt to those targets.
*Payoff:* the entire engine builds and runs its test suite on a headless Linux CI runner in seconds,
with no Qt installed. (This is not hypothetical — it is the exact situation in the sandbox where this
review was written; see §18.3.)

**Rule B — Decoders know nothing about sessions, files, or widgets.** A decoder's entire universe is:
a `SampleWindow` in, a `std::vector<DecodedEvent>` out, plus its own configuration. A decoder must be
constructible and runnable inside a unit test from a `std::vector<uint8_t>` and nothing else.

### 1.3 Build targets

| Target | Type | Depends on | Qt? | Notes |
|---|---|---|---|---|
| `usn::common` | static lib | — | no | `fmt`, `nlohmann/json` isolated to `.cpp` |
| `usn::model` | static lib | common | no | header-heavy, value types only |
| `usn::transport` | static lib | common, model, `shared/wire` | no | platform serial backends |
| `usn::protocol` | static lib | common, model | no | one subdir per protocol |
| `usn::trigger` | static lib | common, model, protocol(types only) | no | AST + 2 back-ends |
| `usn::hal` | static lib | common, model, transport, trigger | no | device abstractions |
| `usn::core` | static lib | all of the above | no | engine |
| `usn::app` | static lib | core + Qt6 Core/Gui/Qml | **yes** | view-models, QML types |
| `usn::gui` | executable | app + Qt6 Quick | **yes** | `qt_add_qml_module` |
| `usn::tools::*` | executables | core | no | CLI diagnostics, converters |
| `usn::tests::*` | executables | per layer | partial | gtest; Qt Test for app |
| `firmware/teensy41` | **separate** | `shared/wire` only | no | not built by desktop CMake by default |

`USN_ENABLE_GUI` is a CMake option that **auto-disables** when Qt 6 is not found, printing a clear
status message rather than failing. Result: one repository, two useful build configurations —
"full product" on a developer workstation, "core + tests" on CI and in constrained environments.

---

## 2. Repository tree

Close to master spec §5, with the deviations marked **← new** / **← moved**.

```text
TooL_For_Sniffer_V1/                     (repo root; product dir name kept as-is)
│
├── CMakeLists.txt                       top-level: options, presets, add_subdirectory
├── CMakePresets.json                    windows-debug/release, linux-debug, core-only, asan/tsan
├── README.md
├── LICENSE
├── .clang-format                        enforced style
├── .clang-tidy
├── .editorconfig
├── .gitignore
│
├── cmake/                            ← new
│   ├── UsnCompilerWarnings.cmake      warning sets per compiler, /WX, -Werror
│   ├── UsnOptions.cmake               option definitions + capability probes
│   ├── UsnDependencies.cmake          FetchContent: fmt, json, gtest, benchmark
│   ├── UsnQt.cmake                    Qt6 discovery, qml module helpers
│   ├── UsnSanitizers.cmake
│   └── UsnLayerCheck.cmake            Rule A enforcement
│
├── shared/                           ← new  (single source of truth, host + firmware)
│   └── wire/
│       ├── usn_wire.h                 C-compatible packet header/type layout
│       ├── usn_wire_crc.h             CRC32C (table, constexpr-generated)
│       └── usn_wire_rle.h             RLE codec spec + reference impl (host-side)
│
├── docs/
│   ├── architecture_review.md         this document
│   ├── architecture.md                accepted architecture (distilled from this review)
│   ├── capture_protocol.md            wire format, byte-exact, with worked examples
│   ├── file_format.md                 .usn container
│   ├── decoder_api.md
│   ├── trigger_engine.md
│   ├── performance.md                 measured numbers ONLY, with hardware/date/host records
│   ├── development.md                 build, test, phase gates, contribution rules
│   ├── hardware_constraints.md        §16 expanded, with pin map + measurement records
│   └── adr/                        ← new  Architecture Decision Records
│       ├── 0001-qt-free-core.md
│       ├── 0002-sample-index-primary-key.md
│       ├── 0003-transport-usb-cdc-first.md
│       └── 0004-cpp20-floor.md
│
├── desktop/
│   ├── CMakeLists.txt
│   ├── app/                           Qt view-models, commands, workspace, settings
│   │   ├── Application.{h,cpp}
│   │   ├── models/                    ChannelModel, TransactionTableModel, HexModel,
│   │   │                              DeviceModel, DiagnosticsModel, MeasurementModel
│   │   ├── viewmodels/                CaptureViewModel, WaveformViewModel, TriggerViewModel
│   │   ├── commands/                  undo/redo command stack
│   │   └── QmlRegistration.cpp
│   ├── core/
│   │   ├── pipeline/               ← new  CapturePipeline, fan-out dispatch, backpressure
│   │   ├── capture/                   CaptureSession, CaptureConfiguration
│   │   ├── timeline/                  Timeline, Timebase, ClockDomain
│   │   ├── analysis/                  binary analysis: histogram, entropy, patterns
│   │   ├── measurements/              frequency, period, duty, bitrate, utilization
│   │   ├── search/                    expression lexer/parser/evaluator
│   │   ├── storage/                   .usn reader/writer, chunk index, mmap
│   │   ├── replay/                    ReplaySource (file → pipeline, as if live)
│   │   └── diagnostics/               DiagnosticsLog, counters, integrity audit trail
│   ├── common/                       ← moved out of core (was implicit)
│   ├── model/                        ← new (deviation D1)
│   ├── transport/
│   │   ├── ITransport.{h,cpp}
│   │   ├── PacketCodec.{h,cpp}        framer/deframer, resync, CRC verify
│   │   ├── UsbCdcTransport.{h,cpp}
│   │   ├── FileTransport.{h,cpp}      replay/offline analysis
│   │   ├── LoopbackTransport.{h,cpp}  test double, ships in lib not tests
│   │   └── TransportManager.{h,cpp}
│   ├── hal/
│   │   ├── IDevice.{h,cpp}
│   │   ├── ICaptureDevice.{h,cpp}
│   │   ├── ITriggerEngine.{h,cpp}
│   │   ├── DeviceManager.{h,cpp}
│   │   ├── IDeviceProbe.{h,cpp}       discovery strategy
│   │   ├── teensy/TeensyDevice.{h,cpp}
│   │   └── fake/FakeCaptureDevice.*   test double, ships in lib
│   ├── protocol/
│   │   ├── DecoderAPI.{h,cpp}
│   │   ├── DecoderRegistry.{h,cpp}
│   │   ├── DecoderConfiguration.{h,cpp}
│   │   ├── SampleWindow.{h,cpp}
│   │   ├── spi/  i2c/  uart/  can/  lin/  gpio/
│   ├── trigger/
│   ├── waveform/                      decimation pyramid, LOD, chunk cache (C++ side)
│   ├── hex/                           hex/bit/interp models + diff engine
│   ├── export/                        csv/json/bin/vcd/pcap writers
│   ├── import/                     ← new (spec folded import into export/)
│   ├── scripting/                     (Phase 14)
│   ├── plugins/                       (Phase 14)
│   ├── diagnostics/                   diagnostics bundle collection (Phase 18)
│   └── gui/
│       ├── main.cpp
│       ├── qml/                       Main.qml, panels/, theme/
│       ├── components/                WaveformView (QQuickItem + RHI nodes)
│       ├── windows/
│       └── rendering/                 scene-graph node/material implementations
│
├── firmware/
│   └── teensy41/
│       ├── CMakeLists.txt             optional; requires arm-none-eabi / Teensyduino
│       ├── platformio.ini             alternative toolchain (decision pending, §17 R7)
│       ├── src/                       main.cpp, capture_state_machine.* (HOST-TESTABLE)
│       ├── hal/                       pin map, clock config, memory placement (OCRAM/PSRAM)
│       ├── capture/                   GpioSampler, SampleClock, RingBuffer, OverflowMonitor
│       ├── dma/                       DmaChannel wrapper, TCD setup, ping-pong
│       ├── usb/                       UsbLink, PacketFramer, BackpressureMonitor
│       ├── trigger/                   device-side trigger subset
│       ├── rle/                       device-side RLE encoder
│       └── tests/                     host-compiled unit tests for state machine + framer
│
├── tests/
│   ├── unit/                          per-layer gtest suites
│   ├── integration/                   fake device → pipeline → .usn → decoders → models
│   ├── protocol/                      decoder tests driven by golden files
│   ├── transport/                     codec fuzz + corruption/gap/resync injection
│   ├── performance/                   Google Benchmark + end-to-end scenarios
│   ├── stress/                        long-run, larger-than-RAM, thread soak
│   ├── golden/                        {input.bin, config.json, expected.json} triples
│   └── fakes/                         shared test doubles (FakeTransport, WaveformBuilder)
│
├── tools/
│   ├── signal_generator/              host-side synthetic capture generator (no hardware)
│   ├── capture_converter/             .usn ⇄ bin/csv/vcd/pcap
│   ├── wire_dumper/                ← new  decode a raw byte stream → human-readable packets
│   └── diagnostics/                   collect environment + logs into a bundle
│
├── .github/workflows/                ← new
│   ├── ci-core.yml                    no-Qt, 3 compilers, sanitizers — always runs
│   ├── ci-gui.yml                     Qt 6 build + offscreen QML smoke tests
│   └── ci-firmware.yml                firmware build (compile-only; HIL is manual)
│
└── third_party/                       only for deps that cannot be FetchContent'd
```

**Files deliberately not created:** no `.vscode/` committed until Phase 1 defines exact tasks; no
`installer/` until Phase 18; no per-protocol subdirectories until the protocol is actually being
written (empty directories are noise); no `docs/*.md` stubs with placeholder content.

---

## 3. Module responsibilities

| Module | Owns | Must NOT know about | Key types | Primary test strategy |
|---|---|---|---|---|
| `common` | error/status model, logging, CRC32C, SPSC queue, ring buffer, byte-order helpers, perf probes, time utilities | everything above it | `Status`, `StatusOr<T>`, `ErrorCode`, `LogCategory`, `SpscQueue<T>`, `Crc32c` | unit (exhaustive), fuzz on queue under contention |
| `model` | immutable value types describing samples, channels, time, events, capabilities | I/O, threads, Qt, protocols' internals | `SampleIndex`, `DeviceTick`, `Timebase`, `ChannelId`, `ChannelDescriptor`, `OwningSampleBlock`, `SampleBlockView`, `DecodedEvent`, `Transaction`, `DeviceCapabilities`, `DiagnosticEvent` | unit: round-trip, invariants, size/alignment static_asserts |
| `transport` | bytes on a link: open/close/read/write, framing, CRC verification, resync, throughput accounting | what the bytes *mean* semantically (no decoding, no capture policy) | `ITransport`, `PacketCodec`, `PacketHeader`, `FramingStats`, `UsbCdcTransport` | unit + fuzz: random/corrupt/truncated streams must never crash or hang; must always resync |
| `hal` | device identity, capability negotiation, configuration, capture lifecycle | GUI, file formats, protocol semantics | `IDevice`, `ICaptureDevice`, `ITriggerEngine`, `IDeviceProbe`, `DeviceManager` | unit with `FakeCaptureDevice`; integration against recorded streams |
| `protocol` | turning samples into events | sessions, storage, transport, GUI | `IProtocolDecoder`, `DecoderInfo`, `DecoderConfiguration`, `SampleWindow`, `DecodedEvent`, `DecoderRegistry` | **golden files** + exhaustive hand-built vectors + fuzz |
| `trigger` | trigger AST, device-subset compilation, host evaluation | how bytes move, how files are stored | `TriggerNode`, `TriggerCompiler`, `TriggerEvaluator`, `TriggerCapabilities` | unit: AST round-trip, compile-target validation, evaluator truth tables |
| `core` | capture sessions, timeline, storage, replay, analysis, measurement, search, diagnostics | widget/QML types, transport byte details (uses `ITransport`) | `CapturePipeline`, `CaptureSession`, `Timeline`, `StorageEngine`, `ReplaySource`, `MeasurementEngine`, `SearchEngine`, `DiagnosticsLog` | unit + integration + stress (larger-than-RAM) + perf |
| `app` | Qt view-models, list models, commands, workspace state, settings persistence | device registers, packet bytes, decoder internals | `CaptureViewModel`, `TransactionTableModel`, `HexModel`, `WaveformViewModel` | Qt Test: roles, `data()`, signal emission, thread-marshalling |
| `gui` | QML layout, interaction, scene-graph rendering | all business logic | `WaveformView`, QML panels | offscreen QML smoke tests, render-node unit tests, manual UX |
| `firmware` | deterministic sample acquisition, buffering, framing, overflow detection, device trigger | host GUI, `.usn` format, protocol decoding (D5) | `CaptureStateMachine`, `GpioSampler`, `RingBuffer`, `PacketFramer`, `OverflowMonitor` | **host-compiled unit tests** for state machine/framer; HIL for timing |

Boundary note on `core` ↔ `protocol`: `core` *drives* decoders (owns threads, feeds windows, collects
events) but contains **no protocol knowledge**. `protocol` never calls into `core`. This is why `model`
must exist as its own layer (deviation D1) — without it, `protocol` would have to include `core`
headers for `DecodedEvent`, creating a cycle.

---

## 4. C++ interface proposal

### 4.1 Language and tooling decisions

| Decision | Choice | Rationale |
|---|---|---|
| Standard | **C++20** enforced floor (`cxx_std_20`), C++23 opt-in per target | D4. `std::expected` needs GCC 13/MSVC 19.36; CI matrix must include GCC 12. |
| Exceptions | **No exceptions across module boundaries.** `noexcept` on moves/dtors. Exceptions permitted only inside `gui`/`app` for Qt interop and config parsing, caught at the seam. | Capture path must be predictable; a `bad_alloc` mid-capture must degrade to a diagnostic, not a crash. |
| Error propagation | `usn::Status` / `usn::StatusOr<T>` | Hand-rolled minimal (~150 LOC) with `USN_HAVE_STD_EXPECTED` detection so it can become `std::expected` later without API churn. |
| Formatting | `{fmt}` vendored, wrapped as `usn::format` | Becomes `std::format` where available. Needed for logging without pulling in Qt. |
| JSON | `nlohmann/json`, includes isolated to `.cpp` files | Config + `.usn` extensible metadata. MIT, header-only. |
| Ownership | `std::unique_ptr` default; `std::shared_ptr` **only** for immutable snapshots crossing thread boundaries; `std::span` for borrowed views; move-only blocks in queues | Master spec §8. The one `shared_ptr` justification: GUI snapshots consumed by the render thread. |
| Naming | `PascalCase` types, `camelCase` functions/variables, `k`-prefixed constants, **`m_`-prefixed members**, `I`-prefixed pure interfaces | The `m_` prefix is deliberate, not decorative: with `-Wshadow -Werror` it makes member/parameter collisions visible at a glance, and it matches Qt's own convention, which matters because `app`/`gui` must interoperate with Qt types. |
| Warnings | MSVC `/W4 /WX /permissive- /Zc:__cplusplus /utf-8`; GCC/Clang `-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wuseless-cast -Werror`; third-party `/external:W0` / `-isystem` | Master spec §49 requires warning review. |

### 4.2 `common` — status and error model (master spec §43)

```cpp
namespace usn {

enum class ErrorCode : uint16_t {
    Ok = 0,
    // DEVICE
    DeviceNotFound, DeviceBusy, DeviceUnsupported, DeviceResetDetected,
    DeviceFirmwareIncompatible, DeviceConfigurationRejected,
    // USB / TRANSPORT
    UsbOpenFailed, UsbReadFailed, UsbWriteFailed, UsbTimeout, UsbDisconnected,
    // FRAMING / INTEGRITY
    CrcMismatch, InvalidMagic, InvalidPacketLength, HeaderVersionUnsupported,
    SequenceGap, StreamIdMismatch, FramingResync,
    // CAPTURE / BUFFERS
    CaptureNotArmed, CaptureAlreadyRunning, DmaOverflow, RingBufferOverflow,
    HostQueueOverflow, SampleRateUnsupported, ChannelCountUnsupported,
    // DECODER / PROTOCOL
    DecoderNotFound, DecoderConfigurationInvalid, DecoderInternalError,
    ProtocolFramingError, ProtocolChecksumError, ProtocolTimeout,
    // FILE
    FileOpenFailed, FileReadFailed, FileWriteFailed, FileFormatUnsupported,
    FileCorrupt, FileTruncated, FileVersionUnsupported, IndexRebuilt,
    // SYSTEM
    PluginLoadFailed, PluginApiMismatch, ScriptError, ScriptSandboxViolation,
    ConfigurationError, OutOfMemory, Cancelled, NotImplemented, Unknown
};

enum class ErrorSeverity : uint8_t { Info, Warning, Error, Critical };

class Status {
public:
    Status() noexcept;                                   // Ok
    static Status ok() noexcept;
    static Status error(ErrorCode, std::string message);
    Status& withContext(std::string key, std::string value);   // e.g. {"streamId","7"}
    Status& withSampleIndex(SampleIndex);

    bool ok() const noexcept;
    explicit operator bool() const noexcept;
    ErrorCode code() const noexcept;
    ErrorSeverity severity() const noexcept;             // derived from code by table
    std::string_view message() const noexcept;
    const std::vector<ContextEntry>& context() const noexcept;
    std::string toString() const;                        // stable, loggable, testable
private:
    ErrorCode m_code{ErrorCode::Ok};
    std::string m_message;
    std::vector<ContextEntry> m_context;
    std::optional<SampleIndex> m_sampleIndex;
};

template <class T> class StatusOr;   // value_or_status, checked access, no exceptions

} // namespace usn
```

Every error carries enough context to diagnose without a debugger: code, human message, key/value
context, and — where applicable — the `SampleIndex` and `streamId` at which it occurred.
`DiagnosticEvent` (§10.5) is the *recorded* form of a `Status`; `DiagnosticsLog` is the audit trail
required by master spec §14 ("never silently discard data").

### 4.3 `common` — logging (master spec §44)

```cpp
namespace usn::log {

enum class Level : uint8_t { Trace, Debug, Info, Warning, Error, Critical };

struct Category { const char* name; };          // compile-time, zero allocation
inline constexpr Category kApp      {"app"};
inline constexpr Category kDevice   {"device"};
inline constexpr Category kUsb      {"usb"};
inline constexpr Category kCapture  {"capture"};
inline constexpr Category kDecoder  {"decoder"};
inline constexpr Category kStorage  {"storage"};
inline constexpr Category kPlugin   {"plugin"};
inline constexpr Category kScript   {"script"};
inline constexpr Category kPerf     {"perf"};

struct Record {
    Level        level;
    const char*  category;
    std::string  message;          // already formatted
    TimestampUtc wallClock;        // human reference only
    uint64_t     monotonicNs;      // for ordering/latency
    std::thread::id thread;
};

class ISink { public: virtual ~ISink() = default; virtual void write(const Record&) = 0; };
class RingBufferSink : public ISink {};    // feeds the in-app Diagnostics panel, bounded
class FileSink       : public ISink {};    // rotating, flush policy configurable
class QtSink         : public ISink {};    // lives in usn::app — the ONLY Qt-aware sink

class Logger {   // thread-safe; per-category runtime level filtering; lock-free hot path
public:
    static Logger& instance();                          // one justified process singleton
    void setLevel(Category, Level);
    Level level(Category) const noexcept;
    void addSink(std::shared_ptr<ISink>);
    void log(Level, Category, std::string_view formattedMsg);
};

} // namespace usn::log

#define USN_LOG(level, cat, ...) \
    do { if (::usn::log::Logger::instance().level(cat) >= ::usn::log::Level::level) \
             ::usn::log::Logger::instance().log(::usn::log::Level::level, cat, ::usn::format(__VA_ARGS__)); \
    } while (false)
// USN_LOG_INFO(usn::log::kCapture, "started stream={} rate={}Hz", id, rate);
```

The level check precedes formatting, so disabled logs cost one atomic load. The macro is the single
permitted macro-heavy construct; it exists because a function call cannot lazily skip formatting.

### 4.4 `model` — time, samples, capabilities (master spec §10, §12, §15)

```cpp
namespace usn {

// ---- Strong integer types (no implicit conversion to/from integers) ----
struct SampleIndex {
    uint64_t value{};
    constexpr SampleIndex() = default;
    constexpr explicit SampleIndex(uint64_t v) : value(v) {}
    friend constexpr auto operator<=>(SampleIndex, SampleIndex) = default;
};
struct DeviceTick { uint64_t value{}; /* same treatment */ };
struct ChannelId  { uint16_t value{}; static constexpr ChannelId invalid(); };

// ---- Timebase: exact rational, integer-only ----
struct Timebase {
    // One tick == numerator/denominator seconds. Never floating point.
    uint64_t numerator{1};
    uint64_t denominator{1'000'000'000};   // default: nanosecond ticks
    // Conversion to display units happens ONLY at the presentation edge,
    // via RationalTime -> long double / double, and the UI must also show SampleIndex.
};
struct RationalTime { int64_t numerator; uint64_t denominator; };

RationalTime sampleIndexToTime(SampleIndex since, SampleIndex at, uint64_t sampleRateHz);
RationalTime tickToTime(DeviceTick since, DeviceTick at, const Timebase&);

// ---- Channel configuration ----
struct ChannelDescriptor {
    ChannelId   id;
    std::string name;              // "SPI1_CLK"
    uint32_t    physicalPin;       // device-reported, for the pin map / diagnostics
    uint8_t     bitPosition;       // bit within the sample word
    bool        enabled{true};
    bool        inverted{false};
    uint32_t    colorArgb{0xFF00FF00};   // presentation hint, ignored by core logic
};
using ChannelMap = std::vector<ChannelDescriptor>;   // ordered by bitPosition

// ---- Sample blocks: borrowed view + owning block (master spec §8, §12) ----
struct BlockHeader {
    uint64_t  firstSampleIndex;
    DeviceTick firstTick;
    uint32_t  sequence;
    uint32_t  streamId;
    uint32_t  sampleCount;
    uint16_t  channelCount;
    uint8_t   strideBytes;         // 1, 2, or 4 — packed native width, never widened
    uint8_t   flags;               // compressed, overflowBefore, ...
    uint64_t  channelMask;         // 64-bit: allows two GPIO words in future
    uint64_t  sampleRateHz;
};

class SampleBlockView {            // borrowed, non-owning, cheap to copy
public:
    SampleBlockView(const BlockHeader&, std::span<const std::byte> payload);
    const BlockHeader& header() const noexcept;
    size_t size() const noexcept;                 // == header().sampleCount
    uint32_t wordAt(size_t i) const noexcept;     // stride-aware read, zero-copy
    bool bitAt(ChannelId, size_t i) const noexcept;
    SampleIndex sampleIndexOf(size_t i) const noexcept;
    Status validate() const;                      // length/stride/count consistency
};

class OwningSampleBlock {          // move-only; the unit that flows through queues
public:
    BlockHeader header;
    std::vector<std::byte> payload;   // Phase 1. Later: pooled/recycled buffers,
                                      // measurement-gated (master spec §12).
    OwningSampleBlock(OwningSampleBlock&&) noexcept;
    OwningSampleBlock& operator=(OwningSampleBlock&&) noexcept;
    SampleBlockView view() const noexcept;
};

// ---- Device capability model (master spec §10) — device-reported, never assumed ----
struct TriggerCapabilityFlags { /* bitmask: Edge, Level, Pattern, PulseWidth, ... */ };

struct DeviceCapabilities {
    uint16_t      channelCountMax;
    uint64_t      maxSampleRateHz;           // DEVICE-REPORTED / MEASURED, not hardcoded
    uint64_t      maxSampleRateHzPerChannel; // rate×channels envelope point
    std::vector<ChannelCountRatePoint> rateEnvelope;  // {channels, maxRateHz}
    uint16_t      adcChannels;               // 0 if none usable
    uint16_t      adcResolutionBits;
    uint64_t      onboardBufferBytes;        // OCRAM+PSRAM actually available
    uint32_t      supportedTriggers;         // TriggerCapabilityFlags
    std::vector<std::string> supportedProtocols;  // device-side pre-processing only
    uint8_t       usbSpeedClass;             // FullSpeed/HighSpeed/SuperSpeed
    bool          rleSupported;
    bool          deviceTriggerSupported;
    std::string   firmwareVersion;
    std::string   hardwareRevision;
    std::string   deviceSerial;
    uint32_t      wireProtocolVersion;
};

} // namespace usn
```

**Why `strideBytes` instead of always `uint32_t`:** a 32-channel capture at 20 MSPS is 80 MB/s of
`uint32_t` — unwritable to disk and uncacheable. Keeping 8-channel captures at 1 byte/sample is a 4×
memory and I/O saving that directly serves master spec §42 (captures larger than RAM). The cost is
one accessor function, contained entirely inside `SampleBlockView`.

### 4.5 `transport` (master spec §9)

```cpp
namespace usn::transport {

struct LinkInfo {
    std::string name;                 // "COM7" / "/dev/ttyACM0"
    uint64_t    negotiatedBytesPerSec;// measured, updated continuously
    bool        flowControlled;
};

class ITransport {
public:
    virtual ~ITransport() = default;
    virtual Status open(const TransportConfig&) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const noexcept = 0;
    virtual LinkInfo linkInfo() const = 0;

    // Blocking read with timeout. Returns bytes read, or Status. Never throws.
    virtual StatusOr<size_t> read(std::span<std::byte> dst, std::chrono::milliseconds timeout) = 0;
    virtual Status write(std::span<const std::byte> src) = 0;

    // Backpressure signal from the link (host not consuming). Must be observable.
    virtual bool writeWouldBlock() const noexcept = 0;
};

// Framing: pure function over byte streams, no I/O, exhaustively unit-testable.
class PacketCodec {
public:
    explicit PacketCodec(CodecConfig);           // maxBodyBytes, crcRequired, strictMagic

    // Feed arbitrary byte ranges (any split points). Emits whole validated packets.
    void feed(std::span<const std::byte> bytes);

    // Drained by the consumer; packets are moved out.
    std::vector<ParsedPacket> takePackets();

    const FramingStats& stats() const noexcept;  // resyncs, crcFails, gaps, bytesSkipped
    void reset() noexcept;

private:
    // Internal state machine: HuntMagic -> ReadHeader -> ValidateHeader
    //                       -> ReadBody -> ValidateCrc -> Emit / Resync
};

struct FramingStats {
    uint64_t bytesReceived, packetsOk, crcFailures, headerVersionRejects,
             lengthRejects, resyncEvents, bytesSkippedDuringResync,
             sequenceGaps, maxGapSize;
};

class TransportManager {   // owns transports, discovery probes, health, reconnect policy
public:
    StatusOr<std::unique_ptr<ITransport>> create(const TransportDescriptor&);
    void setReconnectPolicy(ReconnectPolicy);    // attempts, backoff, jitter
    // Signals reconnect attempts/failures as DiagnosticEvents — never silent.
};

} // namespace usn::transport
```

`PacketCodec` being a pure, I/O-free byte-stream state machine is the most important testability
decision in this layer: every corruption mode in master spec §14 can be injected as a byte vector in a
unit test, with no hardware, no threads, and no timing.

### 4.6 `hal` (master spec §9, §10, §40)

```cpp
namespace usn::hal {

enum class DeviceState { Disconnected, Connecting, Connected, Configuring,
                         Armed, Capturing, Flushing, Stopping, Error };

class IDevice {
public:
    virtual ~IDevice() = default;
    virtual DeviceIdentity identity() const = 0;             // name, serial, fw, hw rev
    virtual StatusOr<DeviceCapabilities> queryCapabilities() = 0;   // ASK, never assume
    virtual Status connect() = 0;
    virtual void disconnect() = 0;
    virtual DeviceState state() const noexcept = 0;
    virtual Status healthCheck() = 0;                        // ping + version + counters
    virtual DeviceCounters counters() const = 0;             // overflows, crc, gaps, resets
};

class ICaptureDevice : public IDevice {
public:
    virtual Status configure(const CaptureConfiguration&) = 0;  // validated against caps
    virtual Status arm() = 0;                                   // incl. trigger arming
    virtual Status start() = 0;
    virtual Status stop() = 0;
    virtual Status abort() = 0;                                 // immediate, discards
    // Blocks are pushed to a sink owned by core; device never buffers unboundedly on host.
    virtual void setSampleSink(ISampleSink*) = 0;               // non-owning, single sink
    virtual CaptureProgress progress() const = 0;               // samples, bytes, rate, drops
};

class ISampleSink {   // implemented by core::CapturePipeline
public:
    virtual ~ISampleSink() = default;
    // Contract: must not block longer than the configured budget; returns Backpressure
    // so the device can decide (and report) rather than silently drop.
    virtual Backpressure onBlock(OwningSampleBlock) = 0;
    virtual void onDiagnostic(DiagnosticEvent) = 0;
};

enum class Backpressure { Accepted, AcceptedWithWarning, RejectedStopCapture };

class ITriggerEngine {
public:
    virtual ~ITriggerEngine() = default;
    virtual StatusOr<TriggerCapabilityFlags> capabilities() const = 0;
    virtual Status compile(const trigger::TriggerNode& ast) = 0;  // rejects unsupported nodes
    virtual Status arm() = 0;
    virtual Status disarm() = 0;
    virtual TriggerState state() const = 0;     // Armed/Fired/position/prePostSamples
};

class IDeviceProbe {   // discovery strategy (master spec §40)
public:
    virtual ~IDeviceProbe() = default;
    virtual std::vector<DiscoveredDevice> scan() = 0;
};
// Phase 1: ManualProbe (user supplies port) + NullProbe.
// Phase 3: WindowsProbe (SetupAPI), LinuxProbe (/sys/class/tty), MacProbe (IOKit).

class DeviceManager {  // owns probes + devices; emits DeviceAdded/Removed/Changed
public:
    void addProbe(std::unique_ptr<IDeviceProbe>);
    Status rescan();
    StatusOr<ICaptureDevice*> acquire(const DeviceIdentity&);   // non-owning borrow
    std::vector<DeviceSummary> devices() const;
};

} // namespace usn::hal
```

### 4.7 `core` — storage and pipeline

```cpp
namespace usn::core {

class IStorage {
public:
    virtual ~IStorage() = default;
    virtual Status create(const StorageTarget&, const CaptureMetadata&) = 0;
    virtual Status append(const OwningSampleBlock&) = 0;      // chunked, buffered
    virtual Status appendEvent(std::span<const DecodedEvent>) = 0;
    virtual Status appendDiagnostic(std::span<const DiagnosticEvent>) = 0;
    virtual Status finalize() = 0;                            // writes TOC + trailer
    virtual StorageStats stats() const = 0;
};

class ICaptureSource {   // abstraction over "where blocks come from"
public:                   // implemented by hal (live) and by ReplaySource (file)
    virtual ~ICaptureSource() = default;
    virtual Status start(ISampleSink&) = 0;
    virtual Status stop() = 0;
    virtual CaptureMetadata metadata() const = 0;
};

class CapturePipeline {   // owns threads; fan-out with per-sink backpressure policy
public:
    struct SinkBinding { ISink* sink; QueuePolicy policy; size_t capacity; };
    Status attachSource(std::unique_ptr<ICaptureSource>);
    void addSink(SinkBinding);                  // storage / decoder / analysis / gui
    Status start();  Status stop();
    PipelineCounters counters() const;          // per-sink queued, dropped, blocked, lag
};

enum class QueuePolicy {
    BlockProducer,          // storage: never lose data (default)
    CoalesceLatest,         // GUI: never stall capture
    DropOldestWithDiagnostic// analysis: bounded, but every drop is reported
};

} // namespace usn::core
```

The `QueuePolicy` enum is the mechanical enforcement of master spec §14. There is no policy that
drops silently; `DropOldestWithDiagnostic` is required to emit a `DiagnosticEvent` per drop and to
increment a visible counter.

---

## 5. Qt/QML architecture

### 5.1 Version and build integration

- **Qt 6.8 LTS recommended**, 6.5 LTS acceptable minimum. Decision needed from the project owner
  (see §20 Q2) because it affects `qt_add_qml_module` behavior and RHI feature availability.
- QML types registered **declaratively** via `QML_ELEMENT` / `QML_SINGLETON` / `QML_UNCREATABLE`
  macros plus `qt_add_qml_module(... )` with a proper `URI` (`UniversalSniffer.App`,
  `UniversalSniffer.Controls`). No `qmlRegisterType` boilerplate.
- Resources (QML, images, theme) compiled into the binary via the QML module — no runtime file
  hunting, no working-directory assumptions.

### 5.2 MVVM split (master spec §24)

| Layer | Language | Responsibility | Forbidden |
|---|---|---|---|
| `gui` QML | QML/JS | layout, panels, interaction, animation, visual styling, hit-testing | business logic, loops over samples, format parsing, `>20`-line JS functions |
| `app` view-models | C++ | expose state as `Q_PROPERTY`, actions as `Q_INVOKABLE`, changes as signals; marshal core→GUI | direct device/transport/decoder access |
| `core` | C++ | capture, decode, analyse, store | any `Q*` type |

Data flow is strictly one-way for state, one-way for commands:

```text
QML  --(user action)-->  ViewModel Q_INVOKABLE  -->  core Command  -->  core state change
QML  <--(property/signal)--  ViewModel  <--(queued signal, shared_ptr<const Snapshot>)--  core
```

The **Qt-free seam** is crossed exactly once, by queued signal/slot carrying an immutable snapshot:

```cpp
// In usn::app — the ONLY place that knows both worlds.
class WaveformViewModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(WaveformSnapshot* snapshot READ snapshot NOTIFY snapshotChanged)
    Q_PROPERTY(double visibleStartSample READ ... WRITE ... NOTIFY visibleRangeChanged)
    Q_PROPERTY(double visibleSpanSamples READ ... WRITE ... NOTIFY visibleRangeChanged)
public slots:
    void zoomAt(double anchorFraction, double factor);   // view-space in, model-space out
    void panBySamples(double deltaSamples);
private slots:
    // Queued connection from the core analysis thread. shared_ptr<const T> => no data race,
    // no copying, no locking in the GUI thread.
    void onSnapshotReady(std::shared_ptr<const WaveformSnapshot>);
};
Q_DECLARE_METATYPE(std::shared_ptr<const usn::WaveformSnapshot>)
```

`WaveformSnapshot` contains **only pre-decimated, viewport-sized data** (one min/max/transition-count
triple per channel per pixel column). It never contains raw samples. This is what makes master spec
§23's "do not render unnecessary samples" structurally true rather than a matter of discipline.

### 5.3 Models exposed to QML

| Model | Base class | Rows | Notes |
|---|---|---|---|
| `TransactionTableModel` | `QAbstractTableModel` | decoded transactions | virtualized; `canFetchMore`/`fetchMore` for >1M rows; sortable/filterable via a `QSortFilterProxyModel`-equivalent implemented in C++ (proxy must not copy payloads) |
| `HexModel` | `QAbstractTableModel` | bytes (16/32 per row) | lazy byte provider backed by storage chunk reads; never materializes the whole capture |
| `ChannelModel` | `QAbstractListModel` | channels | name, colour, enabled, protocol binding |
| `DeviceModel` | `QAbstractListModel` | discovered devices | capabilities exposed as a nested `QObject` per device |
| `DiagnosticsModel` | `QAbstractListModel` | diagnostic events | bounded ring; oldest evicted **with a visible "N earlier events evicted" marker** (never silent) |
| `MeasurementModel` | `QAbstractTableModel` | measurements | value + unit + min/max/avg/count/stddev |
| `SearchResultsModel` | `QAbstractListModel` | search hits | background-evaluated, cancellable, progress reported |

### 5.4 Waveform rendering strategy (the hard GUI problem)

Four candidate approaches were evaluated:

| Option | Mechanism | Verdict |
|---|---|---|
| A | QML `Canvas` / `Shape` | **Rejected.** Per-sample JS/item overhead; unusable beyond ~10 k primitives. |
| B | `QQuickPaintedItem` + `QPainter` | **Rejected** as primary. CPU raster, no batching; ok for overlays/markers only. |
| C | `QQuickItem::updatePaintNode` + `QSGGeometryNode` line strips | **Fallback.** GPU-batched, no shaders to write. ~1 draw call per channel per LOD tile. |
| D | Custom `QSGMaterial` + vertex shader via Qt 6 **RHI** (D3D11 on Windows) | **Primary.** One draw call for all channels of a tile; digital edges as instanced quads; handles 100 k+ primitives per frame. |

**Recommendation: implement C first (Phase 5a), profile, then upgrade the hot path to D (Phase 5b)**
behind the same `WaveformView` QML type. This follows master spec §12/§17 (do not optimize
prematurely; measure first) while keeping the door open to the high-performance path.

Rendering data path:

```text
core (analysis thread)
  → LOD pyramid (min/max/transition-count per 2^k samples), built progressively, chunk-cached
  → viewport request (start sample, span, pixel width, channels)
  → WaveformSnapshot (columns × channels, immutable, shared_ptr)
  → queued signal → app view-model → QML property
  → WaveformView::updatePaintNode (GUI thread, builds render data)
  → QSG node consumed by Qt render thread
```

Level-of-detail design (this is what makes 10 GB captures interactive):

```
level 0: raw samples                        (1 sample per entry)
level k: 2^k samples per entry, storing {min, max, transitionCount}
```

`transitionCount > 0 && min != max` ⇒ the column is "busy" and must be drawn as a filled band, not a
line. This preserves visual honesty when zoomed out: the user sees activity density, not a misleading
flat line. Levels are built lazily per channel, cached, and persisted into the `.usn` LOD section so
reopening a file does not recompute them.

Overlays (cursors, markers, trigger position, bookmarks, protocol event bands) are drawn as separate
QSG nodes / `QQuickPaintedItem` layers — they are low-count and must not be interleaved with the
high-volume waveform geometry.

### 5.5 Docking / panel architecture (master spec §25)

Pure-QML apps have no `QDockWidget`. Options:

| Option | Assessment |
|---|---|
| **KDDockWidgets** (KDAB) | Mature, Qt Quick support, GPL/commercial dual licence. **Licence review required** — GPL would force the whole product open source. Leading candidate. |
| Custom QML docking framework | Full control, no licence issue, but significant effort and a long tail of edge cases (drag previews, floating windows, save/restore layout). |
| Qt Widgets shell hosting QML panels | Gives `QDockWidget` for free, but abandons the pure-QML architecture and doubles the UI technology stack. |

**Phase 1 decision: neither.** Phase 1 ships a single-window QML shell with a `PanelRegistry` and a
declarative `LayoutDescription` (serializable to JSON), so panels are *already* independent,
addressable, show/hide-able, and reorderable. The docking back-end is then a swap, not a rewrite.
This is recorded as ADR-pending with the licence question explicitly open (§17 R5).

---

## 6. Thread model

### 6.1 Thread inventory

| # | Thread | Owner | Blocking? | Priority | Queues in / out | May stall capture? |
|---|---|---|---|---|---|---|
| T1 | `usn.rx` | `transport` | **Yes** (blocking read with timeout) | Above normal (Windows: `THREAD_PRIORITY_ABOVE_NORMAL`) | out: SPSC → T2 | n/a (source) |
| T2 | `usn.dispatch` | `core::CapturePipeline` | No | Normal | in: SPSC from T1; out: MPSC per sink | **No** — bounded waits only |
| T3 | `usn.storage` | `core::StorageEngine` | Yes (file I/O) | Normal | in: MPSC | Yes → policy `BlockProducer`, which is intentional and reported |
| T4..T4+n | `usn.decode[k]` | `core` | No | Normal | in: MPSC; out: events → T3, T5 | No — `DropOldestWithDiagnostic` if a decoder falls behind |
| T5 | `usn.analysis` | `core` | No | Below normal | in: event/block refs; out: snapshots → T6 | No |
| T6 | `usn.gui` (Qt main) | Qt | Qt event loop | Normal | in: queued signals with `shared_ptr<const Snapshot>` | **Never** |
| T7 | `usn.render` (Qt) | Qt | Qt-managed | Qt-managed | scene graph sync only | Never |
| T8 | `usn.index` | `core` | Yes | Lowest / idle | background search index, LOD pyramid build | Never |

### 6.2 Flow

```text
Teensy 4.1 (DMA + timer, zero CPU per sample)
   │  USB CDC bulk
   ▼
T1 usn.rx ──── blocking read ────► PacketCodec (feed/validate/resync)
   │                                     │  invalid → DiagnosticEvent (never silent)
   │                                     ▼
   │                            OwningSampleBlock (move-only)
   ▼                                     │
SPSC queue (bounded, e.g. 256 blocks) ───┘
   ▼
T2 usn.dispatch ── sequence gap check ── streamId check ── counters
   │
   ├──► MPSC ──► T3 usn.storage   (chunked .usn append, CRC per chunk, index)
   ├──► MPSC ──► T4 usn.decode[0] (SPI)   ─┐
   ├──► MPSC ──► T4 usn.decode[1] (I2C)    ├─► DecodedEvent ──► T3 (event index)
   ├──► MPSC ──► T4 usn.decode[2] (UART)  ─┘                └─► T5 analysis
   ├──► MPSC ──► T5 usn.analysis  (measurements, statistics, binary analysis)
   └──► MPSC ──► T6 usn.gui       (COALESCED: only latest snapshot survives)
                                          │
                                          ▼  queued signal, shared_ptr<const Snapshot>
                                     QML / scene graph (T6 sync → T7 render)
```

### 6.3 Invariants (each is a testable property, not an aspiration)

1. **The GUI path can never stall the RX path.** The T6 binding uses `CoalesceLatest` with capacity 1;
   a wedged GUI causes at most one snapshot to be dropped, and that drop is counted and shown.
2. **Exactly one writer per queue.** SPSC where possible (cheaper, wait-free); MPSC only where fan-in
   is real. No shared mutable state without a documented owner.
3. **No lock is held while performing I/O.** Storage locks only its index structures.
4. **Every bounded queue has an explicit `QueuePolicy`** and a visible counter for
   queued/high-water/dropped/blocked. Tested in `tests/stress/`.
5. **All cross-thread payloads are either move-only ownership transfers or `shared_ptr<const T>`.**
   No raw pointers to mutable data cross a thread boundary. Ever.
6. **Threads are `std::jthread` with stop tokens**, joined in a deterministic order at shutdown
   (T8, T5, T4.., T3, T2, T1) so no thread writes to a destroyed sink.
7. **Decoders are single-threaded per instance.** Parallelism is across decoder instances /
   channel groups, never inside one decoder. This keeps decoder code simple and race-free.

### 6.4 Verification

`tests/stress/thread_model_test.cpp` runs the pipeline with: (a) an artificially slow storage sink,
(b) an artificially slow decoder, (c) a GUI sink that stops consuming entirely, (d) a transport that
injects corruption and gaps. Assertions: no deadlock within a timeout, no silent loss (every drop
produces a diagnostic), RX counters monotonic, sanitizer-clean under TSan.

---

## 7. Teensy firmware architecture

### 7.1 Toolchain decision

| Option | Pros | Cons | Verdict |
|---|---|---|---|
| **Teensyduino (Arduino core for T4.1)** | Maintained USB CDC stack, `DMAChannel` wrapper, `IntervalTimer`, XBAR helpers, trivial flashing via `teensy_loader_cli`; huge community knowledge base | Arduino-ish globals; some abstraction overhead | **Phase 2 choice** |
| NXP MCUXpresso SDK + bare metal | Full control of USB descriptors → can implement a vendor bulk endpoint for higher throughput | Weeks of bring-up; USB stack is the riskiest part | Deferred; `ITransport` makes the switch host-side-transparent |
| PlatformIO + Teensy platform | Good CLI/CI story, same core as Teensyduino | Extra layer, occasional version skew with Teensyduino | Use for **CI compile-only builds**; Teensyduino/Arduino IDE for flashing during bring-up |

### 7.2 Firmware module map

```text
firmware/teensy41/
├── src/
│   ├── main.cpp                    setup()/loop() — thin; all logic in modules
│   ├── capture_state_machine.h/.cpp   ★ HOST-COMPILABLE, no HW deps → gtest on desktop
│   ├── firmware_version.h          generated at build time (git describe + date)
│   └── command_handler.h/.cpp      parses host COMMAND packets, dispatches
├── hal/
│   ├── pin_map.h                   ★ THE critical table: Arduino pin → GPIO module/bit,
│   │                                 DMA-accessible? (generated + verified, see §7.5)
│   ├── clock_config.h/.cpp         sample clock source, XBAR routing, achievable divisors
│   └── memory_placement.h          DMAMEM/OCRAM/PSRAM section attributes + runtime probe
├── capture/
│   ├── gpio_sampler.h/.cpp         GPIO_DR → DMA source setup, channel mask application
│   ├── sample_clock.h/.cpp         timer → XBAR → DMAMUX request generation
│   ├── ring_buffer.h/.cpp          ★ HOST-COMPILABLE index math (power-of-two, wrap, gaps)
│   ├── overflow_monitor.h/.cpp     producer/consumer distance, overflow latching, reporting
│   └── rle_encoder.h/.cpp          ★ HOST-COMPILABLE, shares shared/wire/usn_wire_rle.h
├── dma/
│   ├── dma_channel.h/.cpp          TCD setup: src=GPIO_DR (step 0), dst=buf (step 4)
│   ├── ping_pong.h/.cpp            half-buffer interrupts, circular DLAST wrap
│   └── dma_health.h/.cpp           transfer-count cross-check vs. expected
├── usb/
│   ├── usb_link.h/.cpp             CDC write with backpressure detection
│   ├── packet_framer.h/.cpp        ★ HOST-COMPILABLE, shares shared/wire/usn_wire.h
│   └── control_channel.h/.cpp      COMMAND/ACK, HELLO, HEARTBEAT, DIAGNOSTIC
├── trigger/
│   └── device_trigger.h/.cpp       supported subset: edge, level, pattern, count, pre/post
└── tests/                          host-compiled gtest for the ★ modules
```

★ = deliberately hardware-independent so it can be compiled and unit-tested on the desktop with
gtest. This is how firmware logic gets real test coverage without hardware in CI (master spec §45).
The **same** `shared/wire/usn_wire.h` is included by both firmware and desktop, so a layout change
that breaks one breaks the other's `static_assert` at compile time.

### 7.3 Capture state machine

```text
             ┌─────────┐  HELLO/caps   ┌────────────┐
   power-on  │  Boot   │──────────────►│    Idle    │◄────────────┐
             └─────────┘               └─────┬──────┘             │
                                             │ CONFIGURE          │ ABORT / STOP
                                             ▼                    │ / ERROR(clearable)
                                      ┌─────────────┐             │
                                      │ Configuring │──NAK────────┤
                                      └──────┬──────┘             │
                                             │ ACK                │
                                             ▼                    │
                                      ┌─────────────┐             │
                                      │    Armed    │             │
                                      └──────┬──────┘             │
                            trigger enabled  │  trigger disabled  │
                     ┌───────────────────────┴───────────────┐    │
                     ▼                                       ▼    │
          ┌────────────────────┐                  ┌──────────────┐│
          │  PreTriggerFill    │                  │  Streaming   ││
          │  (circular, N pre) │                  └──────┬───────┘│
          └─────────┬──────────┘                         │        │
                    │ TRIGGER FIRED                      │        │
                    ▼                                    │        │
          ┌────────────────────┐                         │        │
          │ PostTriggerDrain   │── post count reached ───┤        │
          └─────────┬──────────┘                         │        │
                    ▼                                    ▼        │
                 ┌──────────┐  buffers empty + acked  ┌─────────┐  │
                 │ Flushing │────────────────────────►│ Stopped │──┘
                 └────┬─────┘                         └─────────┘
                      │ unrecoverable (DMA error, USB dead)
                      ▼
                 ┌─────────┐
                 │  Error  │── reports DIAGNOSTIC_EVENT, requires explicit host clear
                 └─────────┘
```

Every transition emits a control packet, so the host never has to *infer* device state. `Flushing` is
distinct from `Stopped` specifically so that "capture ended but data is still in flight" is a visible,
testable condition — the root cause of most truncated-capture bugs.

### 7.4 Data path on device

```text
Timer (GPT/QuadTimer/FlexPWM) ──XBAR──► DMAMUX request
                                          │
GPIO pins ──IOMUX──► GPIO module DR ──────┤  DMA: src step 0, dst step 4,
                                          │  circular (DLAST wrap), half-buffer IRQ
                                          ▼
                     Capture ring buffer in OCRAM (RAM2)  ── or PSRAM if fitted
                                          │
                              half-buffer ISR — with 4 KB halves at 10 MSPS:
                              ≈2.4 kHz @1 B/sample, ≈4.9 kHz @2 B, ≈9.8 kHz @4 B
                                          │  1. compute producer/consumer distance
                                          │  2. if consumer behind → latch OVERFLOW,
                                          │     set flags bit, count it  (never silent)
                                          │  3. optional RLE encode
                                          │  4. build PacketHeader (+CRC)
                                          ▼
                              USB TX queue ──► Serial.write()  (CDC bulk)
                                          │
                              if USB not draining → BLOCK, do not overwrite ring;
                              overflow counter increments; host sees it in the next packet
```

Memory placement is **not** a detail here — it is the design:

| Region | Size | DMA-reachable? | Use |
|---|---|---|---|
| DTCM/ITCM | 512 KB | **No** (not on AXI bus) | stack, control structs, ISR state — never capture buffers |
| OCRAM (RAM2) | 512 KB | **Yes**, DMA-optimized | **primary capture ring** |
| PSRAM (QSPI, optional) | 8 / 16 / 32 MB | Yes, but bandwidth-limited (~44–60 MB/s) | deep burst buffer when fitted; detected at boot, reported in capabilities |
| Flash | 8 MB | n/a | firmware only |

Budget consequences (to be *measured* in Phase 2, computed here for planning):

| Buffer | At 1 B/sample (≤8 ch) | At 4 B/sample (≤32 ch) | Duration @10 MSPS (1 B / 4 B) |
|---|---|---|---|
| 256 KB OCRAM (assumed free after USB/core use, **[M]**) | 262 144 samples | 65 536 samples | 26 ms / 6.6 ms |
| 8 MB PSRAM (optional solder-on) | 8 388 608 samples | 2 097 152 samples | 839 ms / 210 ms |

At 2 MSPS the same buffers give 131 ms / 33 ms (OCRAM) and 4.19 s / 1.05 s (PSRAM). All figures are
`bufferBytes ÷ strideBytes ÷ sampleRate`; the *available* OCRAM figure is the only assumption and must
be measured at boot and reported through `DeviceCapabilities.onboardBufferBytes`.

This is why RLE and the USB ceiling matter more than the sampler.

### 7.5 Phase 2 hardware spike (must run before any rate is promised)

A dedicated, throwaway-quality firmware whose only job is to produce **verified numbers**:

1. Enumerate which Arduino pins map to DMA-accessible GPIO modules; publish `docs/hardware_constraints.md` pin table.
2. Find the maximum contiguous set of channels obtainable from **one** GPIO module word (determines
   whether 16/24/32 channels are capturable in a single 4-byte sample).
3. Sweep sample clock: 100 kHz → as fast as it goes, recording the first missed-sample rate.
   Detection method: feed a known counter pattern (external clock or on-board generated) and verify
   monotonicity — the same technique used in the referenced community experiments.
4. Measure USB CDC sustained throughput with a binary (non-line-based) writer, on the actual target
   host, with `wireshark`/host counters.
5. Measure OCRAM-only vs. PSRAM-enabled maximums.
6. Confirm DMA cannot reach DTCM (expect failure; document it).

**Deliverable:** `docs/hardware_constraints.md` with a measured table `{channels, maxContinuousRate,
maxBurstRate, burstDepth, usbThroughput, host, date, fwVersion}`. Until that table exists, the GUI
shows *"rate not yet characterized"* rather than a number. This is master spec §41 applied literally.

### 7.6 Signal generator firmware (master spec §46)

A second firmware image, `firmware/teensy41/generator/`, that emits **known** SPI / I²C / UART / GPIO
patterns on pins looped back to the capture pins. It produces the golden captures used by HIL tests and
lets us verify decoding, trigger position, timestamp continuity, and channel synchronization against a
ground truth we generated ourselves. It also has a `--pattern=counter@rate` mode used by §7.5.

---

## 8. USB packet protocol proposal

### 8.1 Design goals and one honest caveat

Goals (master spec §13): versioned, extensible, CRC-protected, sequence-numbered, length-validated,
corruption-detectable, loss-detectable, resynchronizable.

**Caveat worth stating plainly:** USB bulk transfers are *already* reliable — the USB protocol has its
own per-packet CRC and retransmission. Our CRC therefore does **not** protect against wire bit-rot.
What it actually protects against is far more likely and far more dangerous:

- firmware DMA/buffer bugs that tear or duplicate samples,
- host-side read/write races that split packets at unexpected boundaries,
- CDC driver or OS buffer anomalies,
- logic errors in the framer itself.

So the CRC's job is **self-validation of our own software**, and its primary companion is the
resync mechanism. The integrity story rests on four legs, in priority order:
**(1) sequence numbers, (2) length validation, (3) magic-based resync, (4) CRC.** Designing it the
other way round (CRC-first) produces a system that reports corruption when the real fault is a framing
bug. This distinction is recorded as an ADR because it changes how failures are diagnosed.

### 8.2 Packet header — 32 bytes, fixed, little-endian

Defined once in `shared/wire/usn_wire.h`, `static_assert`ed on both sides.

```text
off  size  field              description
───────────────────────────────────────────────────────────────────────────────────────────
 0    4    magic              'U','S','N','1'  (0x55 0x53 0x4E 0x31)
 4    1    headerVersion      = 1. Parser accepts >= its minimum, skips unknown trailing bytes
                              using headerLength => forward-compatible header growth.
 5    1    packetType         see §8.3
 6    1    flags              bit0 COMPRESSED_RLE
                              bit1 FIRST_OF_STREAM
                              bit2 LAST_OF_STREAM
                              bit3 OVERFLOW_OCCURRED_BEFORE_THIS_PACKET
                              bit4 CRC_PRESENT
                              bit5 DEVICE_RESET_SINCE_LAST_PACKET
                              bit6 TRIGGER_FIRED_IN_THIS_PACKET
                              bit7 RESERVED (must be 0; non-zero => reject packet)
 7    1    headerLength       = 32 today. Parser reads 32, then skips (headerLength-32) bytes.
 8    4    sequence           u32, per-stream, +1 per packet, wraps. Gap => SEQUENCE_GAP diagnostic.
12    4    bodyLength         u32, bytes following header. Hard max 65536 => a bogus length can
                              never make the host allocate unboundedly. Reject + resync otherwise.
16    8    streamId           u64, assigned by device at capture start (boot nonce ^ counter).
                              Mismatch => STALE_STREAM diagnostic, packets discarded, host
                              re-arms. This is how a device reset is detected mid-capture.
24    4    bodyCrc32c         CRC32C (Castagnoli) over body bytes; 0 if flags.CRC_PRESENT clear
28    4    headerCrc32c       CRC32C over header bytes [0..27]
───────────────────────────────────────────────────────────────────────────────────────────
                             total 32 bytes
```

Two CRCs, not one: a corrupt header must be detectable **before** `bodyLength` is trusted, otherwise a
single bad byte can desynchronize the stream for the rest of the capture or cause a huge allocation.
Cost check: CRC32C slice-by-8 runs on the order of 1 GB/s+ on a Cortex-M7 at 600 MHz and far faster on
the host, i.e. a few percent of the 20 MB/s budget. This will be **measured** in Phase 3, not assumed;
if the firmware-side cost proves material, `headerCrc32c` is retained (it is 32 bytes) and `bodyCrc32c`
becomes optional via `flags.CRC_PRESENT`.

`static_assert(sizeof(PacketHeader) == 32)` plus `static_assert(offsetof(...))` for every field, in the
shared header, compiled by **both** toolchains. A firmware/host layout drift becomes a compile error
instead of a mysterious data corruption.

### 8.3 Packet types

| Type | Name | Dir | Body | Purpose |
|---|---|---|---|---|
| `0x00` | `INVALID` | — | — | Explicitly illegal; used as a resync sentinel in tests |
| `0x01` | `DEVICE_HELLO` | D→H | `DeviceInfo` | Emitted at boot and after reset; carries boot nonce, fw/hw version → device-reset detection |
| `0x02` | `CAPABILITIES` | D→H | `DeviceCapabilities` wire form | Answers master spec §10 — host **asks**, never assumes |
| `0x03` | `COMMAND` | H→D | `{u16 commandId, u16 argLen, args…}` | See §8.6 |
| `0x04` | `COMMAND_ACK` | D→H | `{u16 commandId, u8 status, u16 detailLen, detail…}` | Every command is answered, including failures |
| `0x05` | `CAPTURE_START_ACK` | D→H | `{streamId, firstSampleIndex, deviceTick, sampleRateHz, channelMask}` | Binds the stream to a timebase |
| `0x06` | `CAPTURE_STOP_ACK` | D→H | `{streamId, lastSampleIndex, deviceTick, totals}` | Enables exact end-of-capture reconciliation |
| `0x07` | `SAMPLE_BLOCK` | D→H | see §8.4 | **Main data path** |
| `0x08` | `SAMPLE_BLOCK_RLE` | D→H | see §8.5 | Compressed data path |
| `0x09` | `TRIGGER_EVENT` | D→H | `{streamId, sampleIndex, deviceTick, u16 triggerId, u16 reasonCode}` | Exact trigger position for pre/post alignment |
| `0x0A` | `DIAGNOSTIC_EVENT` | D→H | `{u16 code, u16 severity, u64 sampleIndex, u32 count, u16 detailLen, detail…}` | DMA overflow, ring overflow, backpressure, watchdog, thermal |
| `0x0B` | `HEARTBEAT` | D→H | `{deviceTick, sampleIndex, uptimeMs, txQueueDepth, overflowTotal}` | Liveness + latency/throughput accounting even when idle |
| `0x0C` | `TIME_SYNC` | D→H | `{deviceTick, sampleIndex, hostSendEchoNs}` | Establishes tick↔host monotonic mapping for latency measurement (never for sample timing) |
| `0x0D` | `PING` / `0x0E` `PONG` | both | `{u64 token}` | Round-trip latency, link health |
| `0x0F` | `ERROR` | D→H | `{u16 errorCode, u16 detailLen, detail…}` | Fatal device-side condition |
| `0x10` | `STREAM_END` | D→H | `{streamId, reason, totals}` | Clean termination marker |

Extensibility: unknown `packetType` ⇒ skip `bodyLength` bytes, emit `DIAGNOSTIC` (host side), continue.
Never abort. This makes adding a packet type in firmware v1.1 safe for a host built against v1.0.

### 8.4 `SAMPLE_BLOCK` body

```text
off  size  field
 0    8    firstSampleIndex   u64 — authoritative position key
 8    8    firstDeviceTick    u64
16    4    sampleCount        u32
20    2    channelCount       u16
22    1    strideBytes        u8  — 1 (≤8 ch), 2 (≤16 ch), 4 (≤32 ch)
23    1    reserved           u8  — must be 0
24    8    channelMask        u64 — which physical bits are meaningful
32    8    sampleRateHz       u64 — repeated per block so a rate change is self-describing
40    N    payload            sampleCount * strideBytes, tightly packed, LE
```

Invariants checked by the parser (each rejection is a diagnostic, never silent):
`40 + sampleCount*strideBytes == bodyLength`; `channelCount <= popcount(channelMask)`;
`strideBytes >= ceil(channelCount/8)`; `sampleCount > 0`;
`firstSampleIndex == previous.firstSampleIndex + previous.sampleCount` **or** a `SEQUENCE_GAP`/
`SAMPLE_INDEX_GAP` diagnostic is raised with the exact missing range.

That last check is the heart of master spec §14: sample-index continuity is verified independently of
packet sequence, so a lost packet and a device-side skip are distinguishable.

### 8.5 `SAMPLE_BLOCK_RLE` body

```text
same 40-byte prefix as SAMPLE_BLOCK, then:
40    4    decodedSampleCount  u32 — samples represented after expansion
44    4    runCount            u32
48    ...  runs: { u32 word, u32 count }   (count 1..2^32-1)
```

Rationale: SPI/I²C/UART captures spend most of their time in an idle state. A 10-second idle-heavy
capture at 10 MSPS is 100 M samples; RLE typically reduces that by 10–1000×. This is the single
highest-leverage mitigation for the USB ceiling. `decodedSampleCount` lets the host validate expansion
before allocating, and the decoder refuses to allocate more than a configured cap.

RLE is **specified in Phase 1** (wire format frozen) but **implemented in Phase 3/4 after raw
throughput is measured**, per master spec §12/§17. Freezing the format early avoids a v2 wire format.

### 8.6 Command set (host → device)

| ID | Command | Args | Notes |
|---|---|---|---|
| `0x0001` | `GET_CAPABILITIES` | — | Must be first command after connect |
| `0x0002` | `SET_CAPTURE_CONFIG` | sampleRateHz, channelMask, stride, bufferBytes, mode | Device validates against capabilities → ACK or NAK with reason |
| `0x0003` | `ARM_TRIGGER` | serialized trigger subset | See §12 |
| `0x0004` | `DISARM_TRIGGER` | — | |
| `0x0005` | `START_CAPTURE` | preTriggerSamples, postTriggerSamples (0 = unlimited) | |
| `0x0006` | `STOP_CAPTURE` | — | Graceful: flush then `STREAM_END` |
| `0x0007` | `ABORT_CAPTURE` | — | Immediate, discards; still emits `DIAGNOSTIC` with what was lost |
| `0x0008` | `SELF_TEST` | testId | DMA/XBAR/USB/counter-pattern self checks |
| `0x0009` | `GENERATE_TEST_PATTERN` | patternId, rateHz, durationMs | §7.6 generator, driven over the same link |
| `0x000A` | `GET_COUNTERS` | — | Overflow/CRC/gap/reset totals |
| `0x000B` | `CLEAR_ERROR` | — | Leaves `Error` state |
| `0x000C` | `SET_LED` | state | Bench identification |
| `0x000D` | `RESET_DEVICE` | — | |
| `0x000E` | `ENTER_BOOTLOADER` | — | Firmware update path |

### 8.7 Resynchronization algorithm

```text
state = HUNT_MAGIC
on bytes available:
  HUNT_MAGIC:   scan for 4-byte magic. Bytes skipped → stats.bytesSkippedDuringResync,
                and on first skip of a session emit DIAGNOSTIC(FramingResync, count).
  READ_HEADER:  accumulate 32 bytes (partial reads are normal — CDC gives arbitrary splits).
  VALID_HEADER: check headerVersion supported, headerLength >= 32, flags reserved bit clear,
                bodyLength <= 65536, headerCrc32c matches.
                any failure → stats.headerRejects++, state = HUNT_MAGIC (advance 1 byte).
  READ_BODY:    accumulate bodyLength bytes.
  VALID_BODY:   if flags.CRC_PRESENT → verify bodyCrc32c.
                failure → stats.crcFailures++, DIAGNOSTIC(CrcMismatch, sequence, streamId),
                state = HUNT_MAGIC.
  EMIT:         type-dispatch. Unknown type → skip + diagnostic + continue.
                sequence != expected → DIAGNOSTIC(SequenceGap, expected, got) — packet is STILL
                delivered (partial data beats no data), with the gap recorded.
```

Critical property: **the parser is total**. For any input byte sequence — random, truncated,
adversarial — it terminates, bounds its memory by `65536 + 32`, and never invokes undefined behaviour.
This is verified by a fuzz test in `tests/transport/` running ≥10 M random inputs plus a corpus of
hand-crafted malformed packets.

### 8.8 Physical link choice

See §20 Q1 — this is an open decision with a recommendation:

**Recommendation: Phase 3 ships CDC (`usb_serial`) only.** It is driverless on Windows 10/11, works
with the existing Teensyduino stack, and is enough to characterize the real ceiling. The `ITransport`
abstraction plus the versioned wire format mean a later vendor-bulk (WinUSB/libusb) or Ethernet
(10/100 PHY) back-end is an additive change. Building the high-throughput path first, before knowing
the measured CDC number, risks weeks of USB stack work for an unknown gain — exactly the premature
optimization master spec §12 forbids.

---

## 9. Timestamp strategy

### 9.1 Three clocks, three jobs

| Clock | Type | Source | Used for | Never used for |
|---|---|---|---|---|
| `SampleIndex` | `uint64_t` | device sample counter, +1 per sample | **Primary key.** Ordering, seeking, gap detection, cursor position, event ranges, golden tests | — |
| `DeviceTick` | `uint64_t` | device hardware tick counter + `Timebase` | Exact rational time math; drift measurement | Direct display (needs conversion) |
| `WallClockUtc` | `int64_t ns` | host, at `CAPTURE_START` | Human reference ("captured 2026-09-14 13:22 UTC") | **Any arithmetic.** NTP steps and DST make it non-monotonic |
| `HostMonotonic` | `int64_t ns` | host steady clock | Latency, throughput, UI responsiveness measurement | Sample timing |

### 9.2 Rules

1. **`SampleIndex` is authoritative.** Two records describing the same instant must agree on
   `SampleIndex`, not on a converted time value.
2. **No floating point in storage or event records.** Time is `Timebase` (integer ratio) + ticks, or a
   `RationalTime{int64 num, uint64 den}`. Conversion to `double` happens **only** in `app`/`gui`, at
   the presentation edge, and the UI must display the exact `SampleIndex` alongside any formatted time
   so a rounded display value can never be mistaken for ground truth.
3. **Sample rate is stored per block** (§8.4), so a rate change mid-capture remains exactly
   reconstructible. Timeline position is a piecewise-linear function of `SampleIndex`, built from
   block boundaries — not a single global division.
4. **Uniform sample interval is the norm; irregular intervals are representable.** If a future source
   (ADC, Ethernet-derived capture) produces non-uniform timing, it stores per-sample ticks in the same
   block structure. The timeline API is written against `sampleIndexToTime(index)` from day one, so
   this is not a retrofit.
5. **Decoders never see wall-clock time.** They see `SampleIndex` + `sampleRateHz`. UART baud
   estimation therefore works off integer sample counts, which is both exact and reproducible.
6. **`ClockDomain` exists in Phase 1 with exactly one member.** Multi-device / digital+analog
   synchronization (master spec §35) adds domains without changing the type signatures.
7. **Drift is measured, not assumed.** `TIME_SYNC` + `HEARTBEAT` let the host compute the observed
   device tick rate against its own monotonic clock and report ±ppm. Long captures at nominal
   10 MSPS may differ from true 10 MSPS by the crystal tolerance; measurement records must state it.

### 9.3 Timeline API sketch

```cpp
namespace usn::core {
class Timeline {
public:
    explicit Timeline(Timebase, SampleIndex origin, DeviceTick originTick);
    void addSegment(SampleIndex first, uint64_t sampleRateHz);   // piecewise-linear

    RationalTime  timeAt(SampleIndex) const;                     // exact
    SampleIndex   indexAtTime(RationalTime) const;               // exact, floor + remainder
    double        secondsAt(SampleIndex) const;                  // presentation ONLY, documented
    uint64_t      sampleRateAt(SampleIndex) const;
    bool          isContiguous() const noexcept;
    const std::vector<GapRecord>& gaps() const noexcept;         // explicit, never papered over
    ClockDomain   domain() const noexcept;
};
}
```

`gaps()` returning a first-class list is deliberate: a capture with a lost packet must be *visibly*
discontinuous in every view — waveform, hex, transaction table — rather than silently time-shifted.

---

## 10. Capture data model

### 10.1 Layered representation

```text
OwningSampleBlock        move-only; the unit of transfer through queues and into storage
   └─ BlockHeader + std::vector<std::byte> payload (packed, strideBytes wide)

SampleBlockView          borrowed, non-owning, stride-aware accessor over a payload span
   └─ wordAt(i), bitAt(channel, i), sampleIndexOf(i)

SampleWindow             what decoders and measurements consume: a contiguous range of
   │                     samples with its SampleIndex origin and rate — may span blocks
   ├─ asPackedView()     zero-copy when stride == native
   └─ materializeU32()   explicit, opt-in widening for simple decoders / test convenience

LodColumn                {uint8 min, uint8 max, uint16 transitionCount} per channel per pixel column
   └─ the ONLY waveform data that ever reaches the GUI
```

`SampleWindow` is deviation D2. It replaces the master spec's `std::span<const DigitalSample>` because
a `span<uint32_t>` forces every consumer to widen 1-byte samples to 4 bytes — a 4× memory and
bandwidth penalty that would break master spec §42 for exactly the small-channel-count, high-rate
captures that produce the largest files.

### 10.2 Decoded events (master spec §17)

Two granularities, deliberately separated:

```cpp
namespace usn {

enum class EventType : uint16_t {
    // generic
    Edge, LevelChange, PulseWidth, Gap, Idle,
    // SPI
    SpiTransferBegin, SpiTransferEnd, SpiWord, SpiCsAssert, SpiCsDeassert, SpiClockBurst,
    // I2C
    I2cStart, I2cRepeatedStart, I2cStop, I2cAddress, I2cAck, I2cNack, I2cDataByte,
    I2cClockStretch,
    // UART
    UartStartBit, UartDataBits, UartParityBit, UartStopBit, UartBreak, UartFrame,
    // CAN / LIN
    CanFrame, CanIdField, CanDlcField, CanErrorFrame, CanAckSlot, LinBreak, LinSync,
    LinPid, LinFrame,
    // meta
    ProtocolError, DecoderNote
};

enum class EventStatus : uint8_t { Ok, Warning, Error, Incomplete };

struct EventField {                 // typed, self-describing, GUI-renderable without
    std::string name;               // knowing anything about the protocol
    Variant     value;              // int / uint / bytes / string / bool / double(display only)
    uint8_t     bitOffset;  uint8_t bitWidth;   // for bitfield views
    std::string unit;               // "Hz", "V", "" — optional
};

struct DecodedEvent {               // ATOMIC — one thing that happened
    SampleIndex   begin;            // inclusive
    SampleIndex   end;              // exclusive  => duration is exact, integer
    std::string   protocol;         // "SPI", "I2C", "UART"
    ChannelId     primaryChannel;
    std::vector<ChannelId> involvedChannels;
    EventType     type;
    EventStatus   status;
    std::vector<EventField> fields;
    std::vector<std::byte>  payload;   // raw bytes for the hex viewer
    std::vector<uint16_t>   errorCodes;// ProtocolError detail
    uint32_t      decoderId;
    uint32_t      decoderVersion;
};

struct Transaction {                // AGGREGATE — a protocol-level unit of meaning
    uint64_t      id;               // stable within a session, used by the table model
    SampleIndex   begin, end;
    std::string   protocol;
    std::string   bus;              // user-assigned bus name
    std::string   addressOrId;      // I2C addr / CAN id / SPI CS — display string
    Direction     direction;        // Read / Write / Bidirectional / Unknown
    uint32_t      lengthBytes;
    std::vector<std::byte> payload;
    std::vector<EventField> summaryFields;
    EventStatus   status;
    std::vector<uint16_t> errorCodes;
    std::vector<uint32_t> eventIndices;   // into the event store; lazy-loaded
    bool          bookmarked;
    std::string   note;
};

} // namespace usn
```

The GUI renders `Transaction` in the table and `DecodedEvent` bands as waveform overlays **without
understanding either protocol** — it reads `fields[i].name` and `fields[i].value`. Adding CAN in
Phase 10 requires **zero** GUI changes. That is the test of whether master spec §17 was honoured.

### 10.3 Diagnostic events (master spec §14)

```cpp
struct DiagnosticEvent {
    DiagnosticSeverity severity;    // Info, Warning, Error, Critical
    DiagnosticCategory category;    // Device, Usb, Capture, Buffer, Decoder, Storage, Host, Plugin
    ErrorCode          code;
    std::string        message;
    std::optional<SampleIndex> atSample;
    std::optional<uint32_t>    streamId;
    uint64_t           count;       // aggregated repeats (with firstSeen/lastSeen) to avoid flooding
    TimestampUtc       wallClock;
    uint64_t           monotonicNs;
    std::vector<ContextEntry> context;
};
```

Aggregation matters: an overflow at 10 MSPS can occur thousands of times per second. The model stores
`count` + `firstSeen` + `lastSeen` rather than 10 000 rows, so the Diagnostics panel stays usable
**and** the true magnitude is preserved. Nothing is discarded — it is aggregated, and the aggregation
is visible.

### 10.4 Measurements (master spec §34)

```cpp
enum class MeasurementKind { Frequency, Period, DutyCycle, PulseWidth, HighTime, LowTime,
    EdgeInterval, Bitrate, BaudRate, FrameRate, PacketRate, BusUtilization, RiseTime_AnalogOnly };

struct MeasurementSample { int64_t value; uint64_t unitScale; SampleIndex at; };
struct MeasurementResult {
    MeasurementKind kind;  ChannelId channel;  SampleIndex rangeBegin, rangeEnd;
    int64_t min, max, sum;  uint64_t count;
    // stddev computed in integer fixed point (sum, sumOfSquares, count) — no float accumulation
    int64_t stddevScaled;  uint32_t stddevScale;
    Unit unit;  EventStatus status;   // e.g. InsufficientEdges
};
```

`RiseTime_AnalogOnly` exists in the enum and is **permanently unimplementable for digital GPIO
capture** — it is present so the UI can display it greyed out with an explanation, satisfying master
spec §35's "do not pretend digital GPIO capture provides true analog rise/fall-time measurement"
structurally rather than by omission.

### 10.5 Ownership summary (master spec §8)

| Buffer | Allocated by | Owned by | Released by | Copied? | Mutable? |
|---|---|---|---|---|---|
| USB read buffer | `transport` | `transport` (per-thread) | `transport` dtor | No | Yes (transient) |
| `OwningSampleBlock` payload | `transport`/`PacketCodec` | moved: T1→T2→sinks | last sink holding it | **Never** (move-only) | Immutable once emitted |
| Storage write buffer | `StorageEngine` | `StorageEngine` | on flush/dtor | No | Yes |
| Decoder event vectors | decoder | moved into core event store | event store | No (moved) | Immutable after store |
| `WaveformSnapshot` | `core::analysis` (T5) | `shared_ptr<const>` | refcount (GUI + core) | No | **Immutable** |
| LOD pyramid | `core` (T8) | `core`, cached/mmap'd | cache eviction | No | Immutable per level |
| `.usn` mmap region | `StorageEngine` | `StorageEngine` | RAII unmap | No | Read-only mapping |

The one `shared_ptr` in the system is the immutable GUI snapshot, and it is justified: two threads
(T5 producer, T6 consumer) need concurrent read access with independent lifetimes. Everything else is
`unique_ptr`/move/`span`.

---

## 11. Decoder API

### 11.1 Interface

```cpp
namespace usn::protocol {

inline constexpr uint32_t kDecoderApiVersion = 1;

struct DecoderInfo {
    uint32_t             id;             // stable numeric id, used in files and plugins
    std::string          name;           // "SPI"
    std::string          version;        // "1.0.0"
    uint32_t             apiVersion;     // must equal kDecoderApiVersion
    std::string          description;
    std::vector<std::string> authors;
    std::vector<ChannelRole> requiredRoles;   // e.g. {SCK, MOSI, MISO, CS}
    std::vector<ChannelRole> optionalRoles;
    uint32_t             capabilities;        // bitmask: LazyDecode, Streaming, NeedsBackfill,
                                              //        ProducesTransactions, SupportsErrorInjection
    std::vector<ConfigParameterDescriptor> parameters;   // drives the config UI generically
};

struct DecoderConfiguration {
    uint32_t decoderId;
    std::vector<ChannelBinding> bindings;      // role -> ChannelId
    std::vector<ConfigValue>    parameters;    // typed: bool/int/enum/double/bytes/string
    SampleIndex startHint;                     // where to begin
    bool allowPartialFrames{true};
};

class IProtocolDecoder {
public:
    virtual ~IProtocolDecoder() = default;

    virtual DecoderInfo info() const = 0;

    // Must validate fully and return a precise Status on failure. Must not partially apply.
    virtual Status configure(const DecoderConfiguration&) = 0;
    virtual StatusOr<DecoderConfiguration> configuration() const = 0;

    virtual void reset() = 0;

    // PRIMARY ENTRY POINT. `window` is borrowed; decoder must not retain it beyond the call.
    // Must be deterministic: same window + same state => same events, always.
    virtual Status process(const SampleWindow& window) = 0;

    // Moves out everything produced since the last call. Never returns references to internals.
    virtual std::vector<DecodedEvent> takeEvents() = 0;

    // For larger-than-RAM / viewport re-decode (master spec §42).
    // Checkpoint = opaque, serializable decoder state at a SampleIndex.
    virtual std::vector<std::byte> saveCheckpoint() const = 0;
    virtual Status restoreCheckpoint(std::span<const std::byte>) = 0;

    // Optional: aggregate atomic events into protocol-level transactions.
    virtual std::vector<Transaction> takeTransactions() { return {}; }

    virtual uint64_t samplesProcessed() const noexcept = 0;
};

// Convenience adapter for decoders/tests that want widened samples.
std::vector<uint32_t> widen(const SampleWindow&);

} // namespace usn::protocol
```

### 11.2 Contract (enforced by a decoder conformance test suite)

Every decoder — first-party or plugin — is validated by `tests/protocol/decoder_conformance_test.cpp`,
instantiated per decoder via a type-parameterized gtest:

1. **Purity:** no Qt headers, no global mutable state, no file/network access, no threads. Verified by
   a link-level test (the decoder target does not link Qt) and a grep test.
2. **Determinism:** processing the same window twice after `reset()` yields byte-identical events.
3. **Bounded memory:** event output for N samples is O(N) with a decoder-declared constant; a decoder
   that buffers unboundedly (e.g. waiting forever for a STOP condition) fails the conformance test.
   Required behaviour: emit an `Incomplete` event with `EventStatus::Incomplete` and self-limit.
4. **Total input handling:** arbitrary/random bit patterns must never crash, hang, or invoke UB.
   Fuzz-tested.
5. **Error honesty:** protocol violations (I²C missing ACK, UART framing error, CAN bit error) must
   produce an event with `status != Ok` and a populated `errorCodes`. **A decoder must never silently
   skip a malformed frame.**
6. **Checkpoint round-trip:** `restoreCheckpoint(saveCheckpoint())` at index *k*, then processing
   *[k, n)*, must yield the same events as uninterrupted processing. This is what makes lazy/parallel
   decode of huge files correct.
7. **Sample-index correctness:** every event's `[begin, end)` lies within the processed window.

### 11.3 Registry and configuration descriptors

```cpp
class DecoderRegistry {
public:
    using Factory = std::function<std::unique_ptr<IProtocolDecoder>()>;
    Status registerDecoder(uint32_t id, std::string name, Factory);   // runtime + plugin path
    StatusOr<std::unique_ptr<IProtocolDecoder>> create(uint32_t id) const;
    std::vector<DecoderInfo> available() const;
    void unregister(uint32_t id);
};

// ConfigParameterDescriptor drives the ENTIRE decoder configuration UI generically:
// the GUI never contains per-protocol knowledge (master spec §17).
struct ConfigParameterDescriptor {
    std::string key, label, tooltip, group;
    ConfigType  type;               // Bool, Int, UInt, Enum, Double, Bytes, String, ChannelRole
    Variant     defaultValue;
    std::optional<Range> range;     // numeric bounds
    std::vector<EnumOption> options;// for Enum (e.g. CPOL 0/1, CPHA 0/1, MSB/LSB first)
    bool required;
    uint32_t    apiVersion;
};
```

Concretely, SPI's CPOL/CPHA/bit-order/word-length/CS-polarity requirements (master spec §19) become
four `EnumOption` lists and one `Int` range — data, not code. I²C's clock-stretch detection (§20) and
UART's parity/inversion/break (§21) likewise. **A new protocol adds no GUI code.**

### 11.4 Threading

One decoder instance is owned by exactly one thread (invariant 7, §6.3). Parallelism is achieved by
instantiating multiple decoders (different buses/protocols) or by splitting a capture into
checkpoint-bounded ranges and decoding them on separate threads, then merging by `SampleIndex`. The
checkpoint API is what makes that second form correct rather than merely fast.

---

## 12. Trigger architecture

### 12.1 One AST, two back-ends

The central idea: a trigger is **data**, not code. A serializable AST is defined once and compiled to
whichever back-end can execute it.

```cpp
namespace usn::trigger {

enum class NodeKind {
    // leaves
    Edge,            // channel, direction: Rising/Falling/Any
    Level,           // channel, High/Low
    Pattern,         // channelMask + expected bits + care mask, at a sample
    PulseWidth,      // channel, min/max ticks, polarity
    Timeout,         // channel, no edge for N ticks
    ByteSequence,    // channel-group, byte pattern with mask
    ProtocolField,   // decoderId, field path, comparison  (HOST ONLY)
    Count,           // child node occurred N times
    SearchExpr,      // reuse the §28 search expression  (HOST ONLY)
    // combinators
    And, Or, Not, Sequence,   // Sequence = ordered children with optional max gap
    // meta
    Always, Never
};

struct TriggerNode {
    NodeKind kind;
    std::vector<TriggerNode> children;
    ChannelId channel;
    uint32_t  mask, expected;
    int64_t   thresholdA, thresholdB;
    std::string fieldPath;          // ProtocolField / SearchExpr
    ComparisonOp op;                // Eq, Ne, Lt, Le, Gt, Ge, In, Contains, Matches
    std::string id;                 // user-visible label
};

// Serializable to/from JSON => saved in workspaces and in .usn files, diffable, reviewable.
StatusOr<std::string> toJson(const TriggerNode&);
StatusOr<TriggerNode> fromJson(std::string_view);

enum class Executability { DeviceAndHost, HostOnly, Unsupported };
Executability classify(const TriggerNode&, const TriggerCapabilityFlags&);
```

```text
                    TriggerNode (AST, JSON-serializable)
                              │
              ┌───────────────┴────────────────┐
              ▼                                ▼
   TriggerCompiler::toDevice()        TriggerEvaluator (host, C++)
   - rejects HostOnly nodes           - full feature set
   - emits compact device config      - runs over stored/replayed samples
   - capability-checked               - used for post-hoc "find where this
              │                         would have triggered"
              ▼                                │
   ITriggerEngine::compile()                   ▼
   → ARM_TRIGGER command                SearchResult / TriggerEvent
```

**Why this matters:** the §30 example —
`CH1 Rising AND SPI CS Active AND MOSI == 0x9F AND MISO == 0xEF` —
contains a `ProtocolField` node (SPI CS/MOSI/MISO semantics). That node is `HostOnly` under D5
(firmware does no protocol decoding). Two honest options, both supported by this design:

- **(a) Express it at the bit level** so the device can execute it: `Edge(CH1,Rising) AND
  Level(CS,Low) AND Pattern(MOSI byte == 0x9F) AND Pattern(MISO byte == 0xEF)`. A byte-level pattern
  match *is* device-executable (shift register compare) without protocol semantics. This is the
  recommended path and covers the vast majority of real use.
- **(b) Use the host evaluator** for post-hoc triggering on a continuous capture, or with a device
  pre-trigger on the bit-level subset.

`classify()` tells the GUI which is which, per node, **before** the user hits Run — with an explanation
of why a node is host-only. The UI never offers a trigger the hardware cannot honour, satisfying master
spec §10 ("never hard-code unsupported hardware capabilities into the GUI") at the level of individual
trigger nodes.

### 12.2 Acquisition modes

| Mode | Pre-trigger | Post-trigger | Device support | Notes |
|---|---|---|---|---|
| Continuous (no trigger) | — | — | Required | Stream until stopped/overflow |
| Single-shot | N samples in circular buffer | M samples or until stopped | Required | Classic LA behaviour |
| Normal / re-arm | re-fills pre-buffer | M samples | Required | Repeats until disarmed |
| Auto | falls back to continuous on timeout | — | Optional | Prevents "no waveform ever" UX |
| Sequential / multi-stage | per stage | per stage | Future | AST already supports `Sequence` |
| Post-hoc (host) | n/a | n/a | Host only | Evaluate against a stored capture |

Pre-trigger depth is bounded by the device ring buffer, so `DeviceCapabilities.onboardBufferBytes`
directly limits it: at 32 ch / 4 B per sample, 256 KB OCRAM gives ≤ 65 536 pre-trigger samples. The
GUI computes and displays this bound from reported capabilities rather than a hardcoded number.

### 12.3 Trigger position reporting

On fire: device emits `TRIGGER_EVENT{sampleIndex, deviceTick, triggerId, reason}` and sets
`flags.TRIGGER_FIRED_IN_THIS_PACKET` on the containing sample block. The host records
`triggerPositionSampleIndex` in the session and in the `.usn` metadata, so every view (waveform marker,
transaction table highlight, hex offset) aligns to the **same integer**. Cursor-at-trigger is then a
one-line operation, and it is exactly reproducible in a golden test.

---

## 13. File format architecture (`.usn`)

### 13.1 Design goals

| Goal (master spec) | Mechanism |
|---|---|
| §36 versioned native format | `formatVersion` + per-section versions + unknown-section skip |
| §42 larger than RAM | fixed-size independently-CRC'd chunks + chunk index + `mmap`/streamed reads + per-chunk decoder checkpoints |
| §36 lazy loading | LOD sections + event index let the UI open a 10 GB file and show waveforms without reading DATA |
| crash safety | chunk-level magic/length/CRC + trailer duplicating the TOC offset → offline recovery scan |
| integrity | per-chunk CRC32C + recorded `DIAGNOSTIC` audit trail + per-section CRC |
| extensibility | section directory; new sections ignored by old readers |

### 13.2 Layout

```text
┌───────────────────────────────────────────────────────────────────────────┐
│ FileHeader (64 bytes, fixed)                                              │
│  0  4  magic 'U','S','N','F'                                              │
│  4  4  formatVersion (u32)                                                │
│  8  4  flags (u32): bit0 COMPRESSED_CHUNKS, bit1 HAS_LOD, bit2 HAS_EVENTS,│
│                     bit3 RECOVERED, bit4 MULTI_SEGMENT, bit5 ENCRYPTED(rsv)│
│ 12  4  headerSize (u32) = 64                                              │
│ 16  8  tocOffset (u64)      ← section directory location                  │
│ 24  4  tocEntryCount (u32)                                                │
│ 28  4  tocEntrySize (u32)   ← fixed-size entries => direct indexing       │
│ 32  8  createUtcNs (i64)                                                  │
│ 40  8  firstSampleIndex (u64)                                             │
│ 48  8  totalSampleCount (u64)                                             │
│ 56  4  headerCrc32c (over bytes 0..55)                                    │
│ 60  4  reserved                                                          │
├───────────────────────────────────────────────────────────────────────────┤
│ Sections (each preceded by a 16-byte SectionChunkHeader when chunked)     │
│   magic 'S','C','H','1' | sectionType u16 | chunkIndex u32 | length u32   │
│   ... payload ... | crc32c u32                                            │
├───────────────────────────────────────────────────────────────────────────┤
│ TOC at tocOffset: array of fixed-size entries                             │
│   { u32 sectionId, u16 sectionType, u16 sectionVersion,                   │
│     u64 offset, u64 length, u32 crc32c, u32 flags }                       │
├───────────────────────────────────────────────────────────────────────────┤
│ Trailer (32 bytes): magic 'U','S','N','E' | tocOffset u64 | tocCount u32  │
│                     | formatVersion u32 | trailerCrc32c u32               │
└───────────────────────────────────────────────────────────────────────────┘
```

### 13.3 Section types

| Type | Name | Content | Access pattern |
|---|---|---|---|
| `0x01` | `META` | `CaptureMetadata`: device info, fw/hw version, channel config, capture config, `Timebase`, trigger AST (JSON), app version | Read once at open. Binary struct for hot fields + JSON blob for extensible/user fields |
| `0x02` | `CHUNK_INDEX` | sorted array of `{firstSampleIndex, sampleCount, dataOffset, dataLength, crc32c, flags, checkpointOffset}` | Read once; enables O(log n) seek to any `SampleIndex` |
| `0x03` | `DATA` | raw packed sample payloads, default 4 MiB chunks, each independently CRC'd and independently decodable given a checkpoint | Sequential append while recording; random-access read while browsing |
| `0x04` | `CHECKPOINTS` | serialized decoder checkpoints at chunk boundaries, per decoder | Enables lazy/parallel/background decode |
| `0x05` | `EVENTS` | `DecodedEvent` blobs, chunked | Sequential scan, or via `EVENT_INDEX` |
| `0x06` | `EVENT_INDEX` | `{firstSampleIndex, eventCount, offset}` per event chunk + per-transaction table | Drives the transaction table's virtualization |
| `0x07` | `LOD` | per-channel level-of-detail pyramids `{level, channelCount, columns[], offset}` | Viewport reads only the levels it needs |
| `0x08` | `MEASUREMENTS` | computed measurement results | Optional, rebuildable |
| `0x09` | `BOOKMARKS` | JSON array | Small, user-edited |
| `0x0A` | `ANNOTATIONS` | JSON array | Small, user-edited |
| `0x0B` | `DIAGNOSTICS` | recorded `DiagnosticEvent`s — **the integrity audit trail** | Always written, even on aborted captures |
| `0x0C` | `SEARCH_INDEX` | inverted index for fast re-query | Optional, rebuildable, background-built |
| `0x0D` | `RECOVERY_LOG` | written during a recovery scan; records what was salvaged | Only present if `flags.RECOVERED` |

### 13.4 Append and crash-recovery behaviour

```text
Recording:
  write FileHeader (tocOffset = UNKNOWN sentinel, flags |= WRITING)
  for each block: buffer until 4 MiB → write SectionChunkHeader + DATA + crc
                  append entry to in-memory CHUNK_INDEX
                  periodically flush a PROVISIONAL TOC + Trailer  ← every N chunks
  on finalize: write EVENTS/LOD/META, write final TOC, write Trailer,
               rewrite FileHeader with real tocOffset, clear WRITING flag

Crash / power loss:
  reopen → header says WRITING or trailer missing/mismatched
  → RecoveryScanner walks the file linearly, validating each SectionChunkHeader
    (magic + length + crc). Valid chunks are kept; the first invalid chunk ends
    the salvaged prefix. Rebuild CHUNK_INDEX + TOC, set flags.RECOVERED,
    append a RECOVERY_LOG section stating exactly how many bytes/samples were
    salvaged and where truncation occurred.
```

The periodic provisional TOC is what bounds data loss to N chunks instead of the whole capture. And
per master spec §14, the recovery is **loud**: the UI shows "This file was recovered; samples after
index X are missing" — never a silently shorter capture.

### 13.5 Metadata encoding decision

Binary struct for hot-path fields (channel config, timebase, counts — read on every open, must be fast
and `static_assert`-checked) **plus** a JSON blob for extensible fields (user annotations, decoder
config, application-specific metadata). Rationale: avoids putting a schema-evolution burden on the
binary layout while keeping the common path allocation-free. Revisit if the JSON blob grows hot;
FlatBuffers/CBOR are the fallback (open decision, low risk).

### 13.6 Import/export (master spec §37)

| Format | Import | Export | Notes |
|---|---|---|---|
| `.usn` | ✔ | ✔ | native |
| BIN (raw packed) | ✔ (requires explicit stride/channels/rate from user) | ✔ | Headerless by nature → the importer must ask, never guess |
| CSV | ✔ (columns → channels) | ✔ | samples-per-row or transitions-per-row, both supported |
| VCD | ✔ | ✔ | natural fit: value-change semantics match our event model |
| PCAP / PCAPNG | **conditional** | **conditional** | See below |
| JSON | — | ✔ (transactions/events) | for tooling integration |
| TXT | — | ✔ | human-readable transaction listing |

**PCAP honesty rule (master spec §37):** PCAP link types are specific. We write PCAP/PCAPNG **only**
where the data genuinely is that protocol:
- CAN frames → `LINKTYPE_CAN_SOCKETCAN` (DLT 227) ✔ legitimate
- Ethernet-captured data (future) → `LINKTYPE_ETHERNET` ✔
- SPI/I²C/UART/GPIO → **refuse**, and explain in the UI: *"SPI is not a PCAP link type; export VCD or
  CSV instead."*

A `LINKTYPE_USER0` (DLT 147) escape hatch is offered **only** behind an explicit
"I understand this is non-standard" toggle, and the export is then labelled in-file. Silently
producing a PCAP that Wireshark will misparse is worse than producing no PCAP.

---

## 14. Testing architecture

### 14.1 Pyramid

```text
                    ▲  HIL (manual, Teensy + generator firmware, §7.6)
                   ▲▲  Performance / stress (labelled, opt-in, long-running)
                 ▲▲▲▲  Integration (fake device → pipeline → .usn → decoders → models)
               ▲▲▲▲▲▲  Protocol golden tests (input.bin → decoder → expected.json)
             ▲▲▲▲▲▲▲▲  Component (transport codec, storage, search, trigger compiler)
           ▲▲▲▲▲▲▲▲▲▲  Unit (common, model, per-decoder, per-module) — the bulk
```

Frameworks: **GoogleTest** for everything Qt-free; **Qt Test** for `app`/`gui`; **Google Benchmark**
for micro-performance. All via `FetchContent` with an offline override
(`FETCHCONTENT_SOURCE_DIR_<name>`) so CI and constrained sandboxes work without network.

CTest labels: `unit`, `component`, `integration`, `protocol`, `perf`, `stress`, `qt`, `hil`, `slow`.
`ctest -L unit` must complete in < 60 s — that is the inner-loop budget, and it is enforced by CI time
reporting.

### 14.2 Test doubles (the linchpin)

Shipped in the libraries, not just in tests, so tools can use them too:

| Double | Purpose | Fault injection it must support |
|---|---|---|
| `LoopbackTransport` | deterministic in-process link | none (baseline) |
| `FakeTransport` | scripted byte stream | truncation, byte corruption, bit flips, split points at every offset, stalls, disconnect mid-packet, garbage prefix, duplicated packets, reordered packets |
| `FakeCaptureDevice` | synthetic captures, no hardware | DMA overflow, ring overflow, sequence gaps, sample-index gaps, device reset, rate mismatch, capability lies (to test host validation) |
| `WaveformBuilder` | construct exact digital waveforms | SPI/I²C/UART/CAN pattern generators with configurable errors (missing ACK, framing error, runt pulse) |
| `FakeStorage` / `TempFileStorage` | file format tests | write failures, disk full, crash-at-offset-N (to exercise recovery) |
| `NullSink` / `CountingSink` / `SlowSink` / `StalledSink` | pipeline backpressure | arbitrary latency, permanent stall |

Every failure mode listed in master spec §14 must have at least one test that injects it and asserts it
is **reported**, not swallowed. This is the mechanical guarantee behind "no silent data loss".

### 14.3 Golden tests (master spec §45)

```text
tests/golden/spi_mode0_basic/
├── input.bin        raw packed samples (stride 1, 4 channels)
├── config.json      channel map + decoder configuration
├── expected.json    expected events + transactions
└── notes.md         what this case proves, and why it exists
```

Runner: load → decode → serialize `actual.json` → compare. JSON is emitted **canonically** (sorted
keys, fixed integer formatting, no floats in event records — see §9) so diffs are meaningful and stable
across compilers and locales. `--update-golden` regenerates, and CI requires the regeneration to be a
separate reviewed commit.

Coverage targets for the initial decoder set:

| Protocol | Mandatory golden cases |
|---|---|
| SPI | all 4 CPOL/CPHA modes × MSB/LSB × word lengths {8,16} × CS active-low/high × multi-CS × back-to-back transfers × runt CS glitch × clock glitch |
| I²C | START/STOP/repeated-START × 7-bit & 10-bit address × read & write × ACK/NACK × clock stretching × multi-byte × bus stuck low × arbitration-loss pattern |
| UART | baud {300…3 Mbps} × data bits {5..9} × stop {1,2} × parity {none,even,odd,mark,space} × inverted × framing error × break × back-to-back bytes × baud auto-estimation |

### 14.4 Fuzz and sanitizer jobs

- `tests/transport/codec_fuzz_test.cpp`: ≥10 M random byte sequences + a curated malformed corpus.
  Assertions: no crash, no hang (per-iteration timeout), memory bounded by 65 568 bytes, no UB.
- `tests/protocol/decoder_fuzz_test.cpp`: random `SampleWindow`s per decoder, same assertions plus
  "output event ranges lie inside the window".
- CI sanitizer matrix: ASan+UBSan on every PR; TSan nightly on the pipeline and stress tests.
  These are the tests that actually catch the thread-model violations in §6.3.

### 14.5 Architecture tests (unusual, but high value here)

- `architecture_layers_test.cpp`: parses the CMake-generated dependency graph; fails on any edge that
  violates §1.1 (Rule A: no Qt below `app`; no `core`→`gui`; no `protocol`→`hal`/`transport`/`core`).
- `wire_format_test.cpp`: `static_assert`s from `shared/wire/usn_wire.h` re-verified at runtime;
  round-trips every packet type; asserts the 32-byte header size and every field offset. Compiled by
  both host and (where possible) firmware CI.
- `no_silent_loss_test.cpp`: for each `ErrorCode` in the `BUFFER_OVERFLOW`/`CRC_ERROR`/`SEQUENCE_GAP`
  families, asserts that a corresponding `DiagnosticEvent` and counter increment are produced.

### 14.6 Qt-layer testing

- `Qt Test` for each model: `rowCount`, `columnCount`, `roleNames`, `data()` for every role, signal
  emission on mutation, `fetchMore`/`canFetchMore` behaviour with >1 M rows.
- QML smoke tests with `QT_QPA_PLATFORM=offscreen`: instantiate every panel, assert no QML errors,
  assert bindings resolve. Cheap, catches most "white screen" regressions.
- Render-node unit tests: feed a known `WaveformSnapshot`, assert the produced `QSGGeometry` vertex
  count and bounds — tests the LOD/decimation logic without a GPU.

### 14.7 Phase gate enforcement (master spec §52)

`docs/development.md` carries the checklist, and CI enforces the automatable parts:

- [ ] `cmake --preset <preset>` configures clean on Windows-MSVC, Linux-GCC, macOS-Clang
- [ ] build with zero warnings (`/WX`, `-Werror`)
- [ ] `ctest -L "unit|component|integration|protocol"` green
- [ ] sanitizers clean (ASan/UBSan on PR; TSan nightly)
- [ ] fuzz jobs complete without findings
- [ ] new/changed public interfaces documented in the relevant `docs/*.md`
- [ ] if a wire/file format changed: version bumped, ADR written, golden files regenerated in a
      separate commit
- [ ] if a performance claim changed: `docs/performance.md` measurement record updated with hardware,
      date, and reproduction command — **or the claim is removed**

---

## 15. Performance measurement strategy

### 15.1 Principle

> A number may appear in documentation only if it was produced by a checked-in, reproducible
> measurement on named hardware, with the record committed alongside it.

No exceptions, no "should be able to", no "up to". Master spec §41 lists forbidden marketing terms;
this section makes compliance mechanical.

### 15.2 Instrumentation (in-product, always on)

```cpp
namespace usn::perf {
class Probe {                       // scoped; RAII; near-zero cost when disabled
public:
    Probe(ProbeId, bool enabled);
    ~Probe();                       // records elapsed + bytes if set
    void setBytes(uint64_t);
};
struct ProbeStats {                 // per ProbeId
    uint64_t count, totalNs, minNs, maxNs;
    uint64_t p50Ns, p95Ns, p99Ns;   // computed from a bounded t-digest-like histogram
    uint64_t bytesTotal;            // → derived throughput
};
class PerfRegistry {                // snapshot to JSON for diagnostics bundles
public:
    std::vector<ProbeStats> snapshot() const;
    Status dumpJson(std::ostream&) const;
};
}
```

Probe points (fixed list, so measurements are comparable across versions):
`transport.read`, `codec.parse`, `codec.crc`, `pipeline.dispatch`, `storage.append`,
`storage.flush`, `decoder.process[<id>]`, `analysis.lod.build`, `analysis.measure`,
`search.evaluate`, `gui.snapshot.publish`, `gui.frame`, `file.chunk.read`, `file.index.seek`.

These feed the in-app **Diagnostics** panel (master spec §25) so a user can report real numbers, and
the Phase 18 diagnostics bundle.

### 15.3 Benchmarks

| Level | Tool | Examples | Gate |
|---|---|---|---|
| Micro | Google Benchmark | CRC32C throughput; RLE encode/decode; `SampleBlockView::wordAt` over stride 1/2/4; LOD build; search-expression evaluation; event serialization | regression vs. stored baseline (±5 % warns, ±15 % fails) |
| Component | gtest + timers | `PacketCodec` MB/s; `StorageEngine` append MB/s and fsync cost; chunk seek latency | absolute thresholds set **after** first measurement |
| End-to-end | `tests/performance/` | Synthetic 1 GB capture: fake device → pipeline → `.usn` → reopen → decode → measure | dropped blocks == 0; wall time recorded |
| GUI | `QSG_RENDER_TIMING=1` + frame hooks | frame time p50/p95/p99 at 1 k / 100 k / 10 M visible transitions; zoom/pan interaction latency; decimation level actually used | p95 frame < 16.6 ms target (to be validated, not promised) |
| HIL | Teensy + generator firmware | max verified sample rate per channel count; USB sustained throughput; overflow behaviour at the ceiling; USB unplug/replug recovery; long-run (≥1 h) stability | recorded in `docs/hardware_constraints.md` |

### 15.4 Measurement record template (mandatory in `docs/performance.md`)

```markdown
### PERF-2026-001 — USB CDC sustained throughput
- Date: 2026-XX-XX
- Host: <CPU model>, <RAM>, <OS build>, <USB controller / port, hub or direct>
- Cable: <length, type>
- Compiler/toolchain: MSVC 19.xx / GCC 13.x, CMake x.y, Ninja z
- Qt: 6.x.y
- Firmware: <git sha>, Teensyduino <version>, CPU clock <MHz>
- Configuration: channels, stride, sample rate, buffer sizes, RLE on/off
- Method: exact command + benchmark name
- Result: mean / p50 / p95 / p99 / min / max, bytes moved, duration
- Anomalies: <anything observed>
- Reproduced by: `ctest -R perf_usb_throughput` / `usn-bench --filter=...`
```

Anything in the docs without such a record gets deleted during review. This is how the project avoids
the "guaranteed 40 MSPS" failure mode master spec §41 warns about.

### 15.5 Optimization discipline (master spec §17, §12)

1. Straightforward implementation first (`std::vector` payloads, `shared_ptr` snapshots, one decoder
   thread).
2. Measure with §15.2 probes and §15.3 benchmarks. Record the baseline.
3. Optimize the measured hot spot only.
4. Re-measure; commit before/after numbers **in the same PR**.
5. Add a regression-guard benchmark so the gain cannot silently disappear.

Candidate optimizations already identified, all deferred until measured:
buffer pooling / recycling for `OwningSampleBlock`; zero-copy `mmap` decode path; RLE on device;
multi-decoder thread scaling; RHI-instanced waveform rendering; chunk-parallel `.usn` writes;
SIMD CRC32C (host) and hardware CRC (RT1062 peripheral — availability to be verified).

---

## 16. Hardware limitations

### 16.1 Verified Teensy 4.1 specification

Sources: PJRC/Adafruit/SparkFun product documentation and the i.MX RT1062 datasheet summaries
retrieved 2026-09-14, plus the SparkFun technical article "Understanding GPIO DMA on Teensy 4.1" and
the PJRC forum throughput threads. Values below are as documented by the vendor; anything marked
**[M]** must be **measured by us** before it appears in user-facing documentation.

| Item | Value | Source class |
|---|---|---|
| MCU | NXP i.MX RT1062, ARM Cortex-M7, dual-issue superscalar, 600 MHz (overclockable ~912 MHz) | vendor doc |
| FPU | hardware, single + double precision | vendor doc |
| RAM | 1024 KB total; 512 KB tightly coupled (DTCM/ITCM); remainder OCRAM | vendor doc |
| Flash | 8 MB (7936 KB usable for programs) | vendor doc |
| Optional expansion | two QSPI pads on the underside; PSRAM 8/16 MB (up to 32 MB with two chips), and/or extra flash | vendor doc |
| EEPROM | 4284 bytes emulated | vendor doc |
| Digital I/O | 55 total, 42 breadboard-friendly | vendor doc |
| PWM | 35 pins | vendor doc |
| Analog in | 18 pins, 12-bit, 2 on-chip ADCs | vendor doc |
| Serial / SPI / I²C / CAN | 8 serial (all with FIFOs), 3 SPI (16-word FIFO), 3 I²C (4-byte FIFO), 3 CAN (1 with CAN-FD) | vendor doc |
| DMA | 32 general-purpose channels | vendor doc |
| USB | device 480 Mbit/s **and** host 480 Mbit/s, native MCU USB (no UART bridge) | vendor doc |
| Ethernet | 10/100 Mbit, DP83825 PHY | vendor doc |
| SD | microSD socket, 4-bit SDIO | vendor doc |
| Other | FlexIO, Pixel Processing Pipeline, peripheral cross-triggering (XBAR), crypto accelerator, TRNG, RTC | vendor doc |
| Logic levels | 3.3 V; **not 5 V tolerant** | vendor doc |
| Power | ~100 mA at 600 MHz | vendor doc |

### 16.2 Limitations that shape the architecture

**L1 — GPIO capture via DMA is slower than CPU GPIO reads.**
The DMA-accessible GPIO path on the i.MX RT1062 is documented as slower than the core's fast-GPIO
path. A community experiment using an externally clocked counter reported reliable operation around
**10 MHz**, with missed samples above that in that specific setup — explicitly *not* a universal hard
limit, since wiring, signal integrity, DMA contention, peripheral clocks, and TCD configuration all
matter. **Consequence:** max sample rate is a **[M]** number from the §7.5 spike. Plan around
1–10 MSPS until measured; do not design the UI around 60 MSPS.

**L2 — Only a subset of GPIO modules are DMA-reachable.**
Capture pins must sit on a DMA-accessible GPIO module. If the desired channels span two modules, a
single 32-bit `DR` read no longer suffices: either two DMA transfers (with the interleaving/atomicity
problem that entails) or a 64-bit sample word at half the rate. **Consequence:** the pin map is a
*design input*, not a user preference. `docs/hardware_constraints.md` will publish a verified table of
"which pins can be captured together, at what rate". The GUI must let the device **reject** an
unsupportable channel selection with a specific reason — this is why `DeviceCapabilities.rateEnvelope`
is a list of `{channels, maxRateHz}` points rather than a single number.

**L3 — DMA cannot access TCM on i.MX RT10xx.**
DTCM/ITCM are not on the AXI bus. Capture buffers must be in OCRAM (RAM2) or PSRAM. **[M]** confirm
empirically in Phase 2 (expect a hard failure if attempted). **Consequence:** effective capture RAM is
≈256–512 KB without PSRAM, i.e. **milliseconds** of burst at high rate/many channels. Deep burst
capture effectively requires the optional soldered PSRAM — a **bill-of-materials requirement**, not a
software tuning knob, and it must be stated in user documentation.

**L4 — PSRAM bandwidth caps buffered sample rate.**
QSPI runs ~105.6 MHz by default (≈44 MB/s raw burst); ~132 MHz is commonly achievable (≈60 MB/s);
8 MB parts have been run to 166.2 MHz. At 4 B/sample that is ≈11–15 MSPS write ceiling, and PSRAM
access contends with flash and bypasses the cache. **[M]** in Phase 2. **Consequence:** PSRAM buys
*duration*, not unlimited *rate*.

**L5 — USB is the sustained bottleneck, and 480 Mbit/s is not 60 MB/s.**
480 Mbit/s is the link signalling rate. The USB 2.0 high-speed **bulk** ceiling is 13 × 512 B per
125 µs microframe × 8000 microframes/s ≈ **53.25 MB/s theoretical**, before any device or host software
cost — and CDC-ACM adds its own framing. Real Teensy measurements in the community range from
~2.5 MB/s (20 Mbit/s reported in one link-layer test) to ~4.8 MB/s, ~6 Mbps, and up to ~16–25 MB/s for
optimized *binary* writers with a cooperative host — heavily dependent on host software efficiency.
PJRC explicitly notes the PC side is usually the limiting factor, and that line-oriented (`println`)
writers are dramatically slower than binary block writes. **Consequence:** plan on **5–20 MB/s [M]**;
treat anything above that as unproven; and the firmware must never use line-oriented output on the data
path. Sustained rate ceiling ≈ measured throughput ÷ bytes per sample:

| Bytes/sample (channels) | @ 5 MB/s | @ 10 MB/s | @ 20 MB/s |
|---|---|---|---|
| 1 B (≤8 ch) | 5 MSPS | 10 MSPS | 20 MSPS |
| 2 B (≤16 ch) | 2.5 MSPS | 5 MSPS | 10 MSPS |
| 4 B (≤32 ch) | 1.25 MSPS | 2.5 MSPS | 5 MSPS |

Mitigations, in leverage order: **RLE (§8.5)** → fewer channels/narrower stride → burst-to-RAM then
download → Ethernet transport (100 Mbit PHY, ≈11 MB/s practical, helps for long cables/off-board
streaming but is not a step change) → custom vendor bulk endpoint (potentially higher, unproven).

**L6 — CDC backpressure stops the device; it does not drop data quietly.**
When the host stops reading, `Serial.write()` eventually blocks. The firmware must detect this and
**latch an overflow diagnostic** rather than overwrite the ring buffer. **Consequence:** a wedged host
application manifests as a captured-but-truncated session with an explicit overflow record — which is
the correct behaviour and is testable via `StalledSink`.

**L7 — 3.3 V logic, no 5 V tolerance, no isolation, no adjustable threshold, no analog front end.**
Directly connecting to a 5 V bus can destroy the pin. There is no programmable input threshold or
hysteresis, so noise margins are fixed by the silicon. No galvanic isolation: ground loops between DUT
and host are a real hazard. **Consequence:** user documentation must mandate level shifting /
buffering / series protection for anything outside a 3.3 V bench setup. The app should surface an
electrical-safety notice when a device connects. This is a hardware truth the software cannot fix, and
it must not be hidden by a polished UI.

**L8 — CAN / LIN / RS485 require external transceivers.**
The RT1062 has CAN controllers (3×, one CAN-FD) but **no PHY**. Per master spec §22, the model is
`MCU GPIO/controller → transceiver → physical bus`. Sniffing CAN at the logic level means observing
transceiver RX/TX, where dominant/recessive encoding, bit stuffing, error frames, and ACK slots are
*already decoded by the transceiver/controller*. **Consequence:** two distinct CAN capture paths must
be modelled honestly — (a) controller-based decode (accurate, but the Teensy participates on the bus),
and (b) logic-level RX sniffing (passive, but sees post-transceiver bits and cannot detect all error
conditions). The decoder and the UI must state which mode is in use. Never present (b) as full CAN
analysis.

**L9 — ADC is not a logic-analyzer substitute.**
12-bit, 2 ADCs, 18 pins, with a practical aggregate rate far below the digital path and no
simultaneous sampling guarantee across channels. **Consequence:** per master spec §35, analog
acquisition is an *extensible future path* with its own `ClockDomain`, and rise/fall-time measurement
is explicitly unavailable for digital GPIO capture (§10.4 keeps the enum entry visible and disabled).

**L10 — Timing accuracy is crystal-limited.**
Sample timing derives from the on-board 24 MHz crystal, typically ±20–50 ppm uncalibrated, with
temperature drift. At 10 MSPS over 10 s, 50 ppm ≈ 5 000 samples of accumulated position error versus a
perfect reference. **Consequence:** long-capture baud/frequency measurements inherit this tolerance.
The `TIME_SYNC` mechanism (§8.3) lets us *report* the observed tick rate and its drift, which is the
honest fix. Measurement results must carry the tick-rate basis they were computed against.

**L11 — Probing limits exceed silicon limits in practice.**
At >5 MSPS, breadboard jumpers ring and ground bounce. Long leads act as antennas. The achievable rate
on a real bench is often set by the fixture, not the MCU. **Consequence:** HIL results (§7.6) must
record the physical setup, and the docs must say so. A "max sample rate" without a stated probing
setup is not a number.

### 16.3 What we will therefore *not* claim

No "40 MSPS guaranteed". No "zero latency". No "unlimited capture". No "32 channels at 50 MHz". No
"PCAP export for SPI". No "entropy analysis proves encryption" (master spec §32 — heuristic output is
labelled heuristic, always). No sample-rate figure anywhere in the UI or docs that is not either
(a) device-reported from measured capabilities, or (b) backed by a §15.4 measurement record.

---

## 17. Risks

| # | Risk | L | I | Mitigation | Detect / phase |
|---|---|---|---|---|---|
| R1 | **USB throughput ceiling is lower than needed** for the target channel count/rate | High | High | §7.5 spike measures it in Phase 2, *before* transport work; RLE; stride narrowing; burst-to-RAM; Ethernet/vendor-bulk fallbacks behind `ITransport` | Phase 2 |
| R2 | **DMA GPIO sample rate / pin-module constraint** invalidates the channel-count target (L1, L2) | High | High | Phase 2 spike produces the verified pin map and rate envelope; `rateEnvelope` is device-reported so the GUI adapts rather than lying | Phase 2 |
| R3 | **Burst depth is milliseconds without PSRAM** (L3) | High | Med | Document PSRAM as a BOM requirement; size OCRAM ring for the continuous-streaming case; pre-trigger depth computed from reported buffer bytes | Phase 2 |
| R4 | **Firmware/host wire-format drift** | Med | High | `shared/wire/usn_wire.h` single source of truth + `static_assert`s compiled by both toolchains + `wire_format_test.cpp` | Phase 1 (mechanism), ongoing |
| R5 | **Qt docking solution unavailable/licence-incompatible** (KDDockWidgets GPL vs commercial) | Med | Med | Phase 1 ships `PanelRegistry` + serializable `LayoutDescription` so docking is a back-end swap; licence question escalated to owner now, not at Phase 5 | Phase 1 design, Phase 5 decision |
| R6 | **Qt version churn** breaks QML module / RHI code | Med | Med | Pin an LTS (6.8 recommended); CI builds against the pinned version; `USN_ENABLE_GUI` off keeps core CI green regardless | Phase 1 |
| R7 | **Teensyduino vs MCUXpresso toolchain** — Teensyduino CDC throughput insufficient, forcing a USB stack rewrite | Med | High | Measure CDC first (R1); `ITransport` isolates the host side; firmware `usb/` module isolated so a descriptor-level rewrite touches one directory | Phase 3 |
| R8 | **Larger-than-RAM decode correctness** — lazy/chunked decode produces different results than whole-capture decode | Med | High | Decoder checkpoint API (§11.1) + conformance test 6 (checkpoint round-trip equivalence) + stress test on a file > RAM | Phase 4/6 |
| R9 | **Golden-test brittleness** — byte-exact JSON diffs break on benign changes | Med | Med | Canonical JSON emission (sorted keys, integer-only event records); `--update-golden` with a separate reviewed commit; `notes.md` per case explains intent | Phase 6+ |
| R10 | **Thread-model violations at the Qt boundary** (data races, GUI stalls) | Med | High | §6.3 invariants; `shared_ptr<const>` only; TSan nightly; `StalledSink` stress test; Rule A keeps core Qt-free so races are reproducible without a GUI | Phase 1 (design), ongoing |
| R11 | **MSVC vs GCC/Clang divergence** (packing, alignment, `offsetof`, endianness assumptions, `/permissive-` strictness) | Med | Med | Explicit `#pragma pack`/`alignas` in wire structs; `static_assert` on size+offset; 3-compiler CI matrix; no bitfields in wire formats | Phase 1 |
| R12 | **Scope creep** — 18 phases, ~40 features; the product never ships | **High** | **High** | §52 phase gates enforced by CI checklist; this review explicitly defers CAN/LIN, plugins, scripting, analog, docking, export formats to their phases; each phase has a written exit criterion | All phases |
| R13 | **No hardware in CI** — firmware and HIL are unverifiable automatically | High | Med | Host-compile all firmware logic that can be (★ modules, §7.2); `FakeCaptureDevice` reproduces every hardware fault mode; HIL results committed as measurement records with fw sha + date | Phase 2+ |
| R14 | **Electrical damage to user hardware / DUT** (L7) | Low | High | Prominent documentation, in-app safety notice on connect, recommended protection circuit in `docs/hardware_constraints.md`; never suggest direct 5 V bus connection | Phase 1 docs |
| R15 | **Single-maintainer bus factor** | Med | High | ADRs, `docs/` per interface, architecture tests that encode the rules, golden `notes.md` explaining *why* each case exists | Phase 1+ |
| R16 | **`.usn` format churn** after real captures exist | Med | Med | Version + section directory + unknown-section skip from v1; recovery scanner; format frozen by ADR before Phase 4 | Phase 4 |
| R17 | **Performance targets set before measurement** (the §41 failure mode) | High | Med | §15.1 principle; no number in docs without a measurement record; GUI shows "not yet characterized" instead of invented specs | Phase 1 policy |
| R18 | **Sandbox/toolchain asymmetry** — the review environment has no Qt, no MSVC, no ARM toolchain, no hardware; GUI/firmware claims cannot be verified here | High | Med | Explicit verification matrix (§18.3) stating what is verified where; never claim an unrun test passed (master spec §49) | Phase 1 |
| R19 | **Third-party dependency vs. CMake-version incompatibility** — *already confirmed for nlohmann/json v3.11.3 under CMake ≥ 4.0* (see D6) | **Confirmed** | Low–Med | Vendor the amalgamated `json.hpp` single header (recommended), or scope `CMAKE_POLICY_VERSION_MINIMUM=3.5`; test configure against both CMake 3.2x and 4.x in CI | P1.3 |
| R20 | **Struct packing / alignment divergence** between MSVC, GCC and arm-none-eabi for the 32-byte wire header (R11's concrete form) | Med | High | Explicit `#pragma pack(push,1)` + `static_assert(sizeof==32)` + `offsetof` asserts for all 15 fields, in `shared/wire/usn_wire.h`, compiled by **all three** toolchains; no bitfields in wire structs; fixed-width `<cstdint>` types only | P1.6 |

---

## 18. Phase 1 implementation plan

### 18.1 Scope (master spec §51, Phase 1 — *foundation only*)

**In scope:** repository structure, CMake + `CMakePresets.json`, MSVC configuration, Qt 6 integration,
QML bootstrap, core interfaces, transport interfaces, HAL interfaces, logging, configuration system,
basic device abstraction, test infrastructure.

**Explicitly out of scope:** waveform analyzer, decoders, triggers, `.usn` writer/reader, hex viewer,
search engine, measurements, export, plugins, scripting, firmware capture logic, USB transport
implementation. Phase 1 delivers **skeletons with tests**, not features.

### 18.2 Work breakdown

| ID | Task | Deliverables | Exit criterion |
|---|---|---|---|
| P1.1 | Build system skeleton | root `CMakeLists.txt`, `cmake/*.cmake`, `.clang-format`, `.gitignore`, `LICENSE` | Configures with Ninja on GCC and (owner machine) MSVC; zero warnings |
| P1.2 | `CMakePresets.json` | `windows-debug`, `windows-release`, `linux-debug`, `linux-release`, `core-only` (no Qt), `asan`, `tsan`; shared hidden base presets | `cmake --preset windows-debug && cmake --build --preset windows-debug` works from CLI **and** VS Code |
| P1.3 | Dependencies | `cmake/UsnDependencies.cmake`: FetchContent for `{fmt}`, `nlohmann/json`, GoogleTest, Google Benchmark; offline override support | Configure succeeds online **and** with `FETCHCONTENT_SOURCE_DIR_*` prepopulated |
| P1.4 | `usn::common` | `Status`/`StatusOr`, `ErrorCode` (full §4.2 enum), `Logger` + categories + sinks, `Crc32c` (constexpr table), `SpscQueue<T>`, `RingBuffer`, `PerfProbe`/`PerfRegistry`, byte-order + span helpers | Unit tests: status/context/toString, log level filtering, CRC32C against known vectors (RFC 3720 test vectors), SPSC under 2-thread contention (TSan-clean), perf probe histogram correctness |
| P1.5 | `usn::model` | `SampleIndex`, `DeviceTick`, `ChannelId`, `Timebase`, `RationalTime`, `ChannelDescriptor`/`ChannelMap`, `BlockHeader`, `SampleBlockView`, `OwningSampleBlock`, `DeviceCapabilities`, `DecodedEvent`, `Transaction`, `DiagnosticEvent`, `MeasurementResult` | Unit tests: strong-type non-convertibility (compile-fail tests), `SampleBlockView` stride 1/2/4 correctness incl. unaligned, `sampleIndexToTime` exactness, `static_assert` sizes, move semantics |
| P1.6 | `shared/wire` | `usn_wire.h` (32-byte header, all packet types, all `static_assert`s), `usn_wire_crc.h`, `usn_wire_rle.h` (spec + host reference impl) | `wire_format_test.cpp`: sizes/offsets, round-trip every packet type, CRC vectors, RLE round-trip incl. edge cases (single run, max run, all-unique) |
| P1.7 | `usn::transport` **interfaces** | `ITransport`, `TransportConfig`, `LinkInfo`, `PacketCodec` (full state machine), `FramingStats`, `LoopbackTransport`, `TransportManager` skeleton, `FileTransport` (read-only) | **Component tests + fuzz:** every corruption/split/truncation case resyncs; memory bounded; no crash over ≥1 M random inputs; stats counters exact |
| P1.8 | `usn::hal` **interfaces** | `IDevice`, `ICaptureDevice`, `ISampleSink`, `ITriggerEngine`, `IDeviceProbe`, `DeviceManager`, `ManualProbe`, `FakeCaptureDevice` (with fault injection) | Unit tests: capability query/validation path, `configure()` rejects out-of-envelope rate/channel combos with a precise `Status`, state machine transitions, counters |
| P1.9 | `usn::protocol` **interfaces** | `DecoderAPI.h` (`IProtocolDecoder`, `DecoderInfo`, `DecoderConfiguration`, `ConfigParameterDescriptor`), `SampleWindow`, `DecoderRegistry`, decoder conformance test harness (no decoders yet) | Registry register/create/unregister; conformance harness compiles and self-tests against a `TrivialEdgeDecoder` stub written purely to validate the harness |
| P1.10 | `usn::core` **skeleton** | `CapturePipeline` (threads, queues, `QueuePolicy`, counters), `Timeline`, `CaptureSession` (state only), `DiagnosticsLog`, `ICaptureSource`, `IStorage` interface | Integration test: `FakeCaptureDevice` → `CapturePipeline` → counting sink; asserts no loss, correct counters, clean shutdown ordering; stress test with `SlowSink`/`StalledSink` proving the GUI path cannot stall RX |
| P1.11 | Configuration system | `Config` schema (JSON), layered load: compiled defaults → user file → workspace → CLI → **device capability clamping**; `ConfigError` reporting | Unit tests: precedence, invalid-value rejection with precise messages, capability clamping (e.g. requested 50 MSPS clamps to reported envelope and emits a diagnostic) |
| P1.12 | Qt 6 integration + QML bootstrap | `usn::app` (`Application`, one `ViewModel`, one `ListModel`, `QtSink`), `usn::gui` (`main.cpp`, `Main.qml` shell, `PanelRegistry`, `LayoutDescription`, theme), `qt_add_qml_module` with URI | Builds with Qt 6; launches; shows a shell window with a live Diagnostics list bound to the core logger; **offscreen QML smoke test passes** |
| P1.13 | CLI tools | `tools/wire_dumper` (bytes → human-readable packets), `tools/diagnostics` (env + log bundle) | `wire_dumper` correctly decodes a captured byte stream and reports framing errors |
| P1.14 | CI | `.github/workflows/ci-core.yml` (3 OS × compiler, no Qt, sanitizers, fuzz), `ci-gui.yml` (Qt build + offscreen), `ci-firmware.yml` (compile-only, deferred until P2) | Green run on the Phase 1 commit |
| P1.15 | Docs | `docs/architecture.md` (accepted distillation), `capture_protocol.md`, `decoder_api.md`, `development.md`, `hardware_constraints.md` (skeleton + measurement-record template), ADR 0001–0004, updated `README.md` | Every public interface from P1.4–P1.11 documented; phase-gate checklist in `development.md` |

### 18.3 Verification matrix — what can be proven where

Stated up front because master spec §49 forbids claiming a test passed when it was not run.

| Deliverable | Verifiable in the review sandbox (Linux, GCC 12, CMake 4.4, Ninja, **no Qt, no MSVC, no ARM, no hardware**) | Requires the owner's Windows machine |
|---|---|---|
| P1.1–P1.3 build system, presets, deps | ✅ configure + build + `ctest` actually run | MSVC-specific flags (`/W4 /WX /permissive-`) compile-checked only |
| P1.4 `common` | ✅ built and unit-tested | — |
| P1.5 `model` | ✅ built and unit-tested | — |
| P1.6 `shared/wire` | ✅ built and unit-tested | — |
| P1.7 `transport` interfaces + codec + fuzz | ✅ built, tested, fuzzed | — |
| P1.8 `hal` interfaces + fakes | ✅ built and unit-tested | — |
| P1.9 `protocol` interfaces + conformance harness | ✅ built and unit-tested | — |
| P1.10 `core` skeleton + pipeline stress | ✅ built, tested, TSan/ASan | — |
| P1.11 configuration | ✅ built and unit-tested | `%APPDATA%` path behaviour |
| P1.12 **Qt 6 + QML bootstrap** | ❌ **cannot compile or run — Qt 6 is not installable here** | ✅ must be built and launched by the owner |
| P1.13 CLI tools | ✅ built and tested | — |
| P1.14 CI | ✅ core job logic validated locally | actual GitHub Actions run |
| P1.15 docs | ✅ written and reviewed | — |

**Consequence for sequencing:** P1.1–P1.11 and P1.13 can be completed and *genuinely verified* in this
environment. P1.12 will be written carefully and correctly against the pinned Qt 6 LTS API, but must be
explicitly labelled **"authored, not compiled"** until built on a machine with Qt 6 — and I will say so
in the report rather than implying it works. This is also the practical reason for deviation D1/D3 (the
Qt-free core): it means ~90 % of Phase 1 is provably correct now instead of hypothetically correct
later.

### 18.4 Phase 1 exit gate (master spec §52)

Phase 2 does not begin until all of the following are true and recorded:

- [ ] `cmake --preset core-only && cmake --build --preset core-only && ctest` green, zero warnings
- [ ] `windows-debug` and `windows-release` presets configure and build on the owner's MSVC machine
- [ ] Qt 6 GUI preset builds and launches on the owner's machine; offscreen QML smoke test passes
- [ ] ASan+UBSan clean; TSan clean on pipeline/stress tests
- [ ] Transport codec fuzz: ≥1 M inputs, no crash, no hang, bounded memory
- [ ] Architecture layer test green (Rule A / Rule B enforced mechanically)
- [ ] `wire_format_test` green with all `static_assert`s
- [ ] All P1.4–P1.11 public interfaces documented in `docs/`
- [ ] ADRs 0001–0004 written and accepted
- [ ] `docs/hardware_constraints.md` contains the measurement-record template and **no invented numbers**
- [ ] No `TODO` without an issue/phase reference; no dead stub files

---

## 19. Architectural change report (master spec §50 format)

Deviations D1–D6 from §0, reported formally.

### D1 — Insert a `model` layer

- **Current architecture:** master spec §4 places Protocol Decoder Engine *below* Core Engine, with no
  home for shared value types.
- **Problem:** `DecodedEvent`, `Transaction`, `SampleBlock`, `DeviceCapabilities` are needed by
  *both* core and protocol. Without a shared layer, `protocol` must include `core` headers, and `core`
  must include `protocol` headers (to drive decoders) → **circular dependency**, which CMake will
  reject and which forces an ad-hoc split later.
- **Proposed change:** extract pure value types into `usn::model`, depending only on `usn::common`.
- **Benefits:** breaks the cycle; makes decoders genuinely standalone (testable from a byte vector
  alone); gives one place for `static_assert`ed layout; makes the Qt-free seam cleaner.
- **Risks:** an extra target; risk of `model` becoming a dumping ground.
- **Compatibility impact:** none — no code exists yet.
- **Testing plan:** `model` unit tests (invariants, sizes, move semantics); architecture layer test
  asserts `model` depends on `common` only.

### D2 — `SampleWindow` instead of `std::span<const DigitalSample>`

- **Current architecture:** master spec §16 proposes `process(std::span<const DigitalSample> samples)`.
- **Problem:** if `DigitalSample` is a fixed-width word (e.g. `uint32_t`), every 8-channel capture must
  be widened 4× before decoding — 4× RAM, 4× disk, 4× cache pressure. That directly defeats §42
  (captures larger than RAM) for exactly the highest-volume configurations.
- **Proposed change:** `process(const SampleWindow&)`, where `SampleWindow` is a stride-aware borrowed
  range with `wordAt(i)` / `bitAt(ch,i)`, plus `materializeU32()` for opt-in widening and a
  `widen()` free function for tests.
- **Benefits:** packed storage end-to-end; decoders stay simple; the widening cost is paid only where a
  decoder author explicitly chooses it.
- **Risks:** `wordAt()` is a function call rather than `span[i]` → potential hot-loop cost.
- **Compatibility impact:** none yet. Master spec §16 anticipates this ("Actual API may evolve after
  Phase 1 design review").
- **Testing plan:** Google Benchmark of `wordAt` vs `span<uint32_t>` indexing at stride 1/2/4;
  conformance tests; golden tests written against `SampleWindow`. **If the benchmark shows >10 %
  decoder slowdown, revisit** (e.g. add a `materializeU32` fast path or template the decoder on stride).

### D3 — Native serial I/O in transport, not `QSerialPort`

- **Current architecture:** unspecified; Qt is the obvious default on a Qt project.
- **Problem:** `QSerialPort` in `transport` makes the core Qt-dependent, requiring Qt (and a
  `QCoreApplication`) for every engine unit test, and breaking headless CI.
- **Proposed change:** `ITransport` with platform back-ends — Win32 `CreateFile` + overlapped I/O,
  POSIX termios. ≈300 lines of well-understood platform code.
- **Benefits:** Rule A holds; core tests run in milliseconds with no Qt; the same code path serves CLI
  tools (`wire_dumper`) that must not need a GUI framework.
- **Risks:** we own platform serial bugs (timeouts, DTR/RTS, buffer sizing) that Qt would have handled.
- **Compatibility impact:** none.
- **Testing plan:** `LoopbackTransport` and `FakeTransport` cover all logic; platform back-ends get a
  thin, separately-tested adapter and are exercised in HIL (Phase 3).

### D4 — C++20 floor rather than C++23

- **Problem:** `std::expected`, `std::print`, `std::mdspan` availability is uneven: GCC 12 (present in
  many CI images and in this sandbox) lacks `std::expected`; MSVC needs 19.36+.
- **Proposed change:** `cxx_std_20` enforced; `USN_CXX_STANDARD` cache variable allows opting a target
  into 23; `StatusOr` is hand-rolled with `USN_HAVE_STD_EXPECTED` detection so it can alias
  `std::expected` later without touching call sites.
- **Benefits:** the CI matrix (MSVC / GCC / Clang) all build the same source; no `#ifdef` sprawl.
- **Risks:** we forgo some C++23 conveniences initially.
- **Compatibility impact:** none; upward-compatible.
- **Testing plan:** 3-compiler CI matrix from P1.14.

### D5 — No protocol decoding in firmware

- **Problem:** an on-device decoder means two implementations of every protocol, guaranteed to diverge,
  and firmware becomes untestable without hardware.
- **Proposed change:** firmware does capture + framing + optional RLE + device-subset trigger only.
  Protocol semantics live on the host, in one place.
- **Benefits:** single source of truth for protocol behaviour; firmware stays small and auditable;
  golden tests cover all decoding.
- **Risks:** protocol-field triggers cannot run on-device (mitigated by §12.1's bit-level expression
  path, which covers most real cases); raw samples must cross USB (mitigated by RLE).
- **Compatibility impact:** none.
- **Testing plan:** trigger `classify()` tests proving the bit-level form is device-executable; HIL
  verification of device trigger position accuracy in Phase 11/16.

### D6 — Vendored third-party deps via FetchContent

- **Problem:** the Qt-free core still needs formatting, JSON, and a test framework.
- **Proposed change:** `{fmt}` (BSL-1.0), `nlohmann/json` (MIT), GoogleTest (BSD-3), Google Benchmark
  (Apache-2.0) via `FetchContent`, pinned by tag, with `FETCHCONTENT_SOURCE_DIR_*` offline override.
- **Benefits:** reproducible, licence-compatible, no submodules to forget, works in restricted
  environments.
- **Risks:** configure-time network dependency (mitigated by the offline override and by CI caching);
  `nlohmann/json` compile-time cost (mitigated by isolating includes to `.cpp` files); **and a verified
  CMake-version blocker, see below.**
- **Compatibility impact:** none.
- **Testing plan:** CI configures both online and offline.

> **Verified during this review (2026-09-14, against the actual upstream sources):**
> CMake ≥ 4.0 rejects any subproject whose `cmake_minimum_required` names a version below 3.5.
> Checking the pinned candidate versions:
>
> | Dependency | Version | `cmake_minimum_required` | CMake 4.x? |
> |---|---|---|---|
> | GoogleTest | v1.15.2 | `3.13` | ✅ OK |
> | `{fmt}` | 11.0.2 | `3.8...3.28` | ✅ OK |
> | Google Benchmark | v1.9.0 | `3.10...3.22` | ✅ OK |
> | nlohmann/json | v3.11.3 | **`3.1...3.14`** | ❌ **configure fails** |
>
> **Resolution required in P1.3, pick one:**
> (i) `set(CMAKE_POLICY_VERSION_MINIMUM 3.5)` scoped around the json subproject (variable available
> since CMake 3.31);
> (ii) vendor the amalgamated single header `json.hpp` into `third_party/` and skip its CMake project
> entirely — **recommended**, it removes the whole class of problem and json is only needed in `.cpp`
> files anyway;
> (iii) pin a future json release that raises its minimum.
> Also note the project must state a `cmake_minimum_required` of at least 3.21 (presets v3 +
> `CMAKE_CXX_STANDARD` handling) and be tested against both CMake 3.2x (common on developer machines /
> VS Code) and CMake 4.x.

---

## 20. Open questions requiring an owner decision

These materially change Phase 1–3 work, so they are asked now rather than discovered later.

**Q1 — USB transport strategy for Phase 3.**
(a) Teensy native CDC serial only — driverless, simplest, ~5–20 MB/s **[M]**;
(b) custom vendor bulk endpoint (WinUSB + libusb on host) — potentially higher ceiling, weeks of USB
stack work, unproven gain;
(c) Ethernet (100 Mbit PHY, ≈11 MB/s practical) — long cables, off-board streaming, no rate step
change;
(d) decide after the Phase 2 measurement spike.
*Recommendation: (d) → (a).* Build CDC, measure, then decide with data.

**Q2 — Qt version floor.** 6.8 LTS (recommended) / 6.5 LTS / whatever is already installed on the
target machine. Affects `qt_add_qml_module` usage, RHI capabilities, and the CI image.

**Q3 — Primary capture target: channel count vs. sample rate.** Per L2/L5 these trade off directly.
(a) ≤8 channels, maximize rate; (b) ≤16 channels, balanced; (c) ≤32 channels, rate secondary;
(d) maximize channels, rate unimportant.
*This determines the Phase 2 pin map and the Phase 3 default stride, and it changes the GUI's channel
configuration UX.*

**Q4 — Phase 1 execution given the sandbox has no Qt 6 / MSVC / ARM toolchain / hardware.**
(a) implement full Phase 1 including Qt/QML code, clearly labelled "authored, not compiled";
(b) implement and *verify* the Qt-free core (P1.1–P1.11, P1.13–P1.15) now with real passing tests,
then add P1.12 once Qt availability is confirmed;
(c) hold all implementation until this review is approved in full.
*Recommendation: (b)* — it produces the most genuinely verified value immediately and keeps §49 honest.

---

## 21. Approval

This review is the gate for Phase 1. On approval:

1. `docs/architecture.md` is written as the accepted distillation (this document remains the rationale
   record).
2. ADRs 0001–0004 are committed.
3. P1.1 → P1.15 execute in order, each with its exit criterion met before the next starts.
4. Phase 2 begins only when the §18.4 exit gate is fully checked, and Phase 2 starts with the §7.5
   hardware spike — because until those numbers exist, no sample-rate decision anywhere in this
   document is more than an estimate.

*End of architecture review.*
