# Universal Sniffer — Architecture Specification

**Status:** Accepted (Phase 1 Baseline)  
**Applies to:** Universal Sniffer Engine & Desktop (v0.1.0+)

---

## 1. Overview & Architectural Goals

Universal Sniffer is an open, cross-platform logic analyzer, protocol decoder, and digital signal sniffer designed for hardware engineers and embedded developers.

The architecture is built to satisfy four primary constraints:
1. **Never silently discard data:** Any buffer exhaustion, transport lag, or packet drop is accounted for, timestamped, and surfaced to the user as a first-class diagnostic event.
2. **Strict Layering & Qt-Free Core (Rule A):** Everything below `desktop/app` is plain C++20 with **zero Qt dependency**. All core engine libraries, transport logic, protocol decoders, and storage primitives are buildable and testable headless in CI without a display server.
3. **Integer-Only Timing (SampleIndex as Primary Key):** Time is derived from sample indices and exact rational fractions, preventing cumulative floating-point rounding errors across multi-gigabyte captures.
4. **Hardware-Honest Rate Envelopes:** Sample rate and channel counts are constrained by empirical hardware ceilings (Teensy 4.1 USB throughput and DMA capabilities), rather than theoretical link speeds.

---

## 2. Dependency DAG & Component Structure

Dependencies point **downward only**. Build-time target linkage mechanically prevents upward or circular dependencies:

```text
                        ┌──────────────────────────────┐
                        │  usn::gui   (QML, resources) │   Qt6::Quick
                        └───────────────┬──────────────┘
                                        │
                        ┌───────────────▼──────────────┐
                        │  usn::app  (view-models,     │   Qt6::Core/Gui/Qml
                        │  QAbstractListModels, cmds)  │
                        └───────────────┬──────────────┘
                                        │  Qt-free seam (Rule A)
        ┌───────────────────────────────▼───────────────────────────────┐
        │  usn::core                                                    │
        │  (CaptureSession, CapturePipeline, Storage, Timeline, Config) │
        └───┬───────────────────────────┬───────────────────────────┬───┘
            │                           │                           │
  ┌─────────▼────────┐        ┌─────────▼────────┐        ┌─────────▼────────┐
  │  usn::protocol   │        │     usn::hal     │        │   usn::trigger   │
  │  (Decoder API)   │        │ (Device Manager) │        │ (Trigger AST)    │
  └─────────┬────────┘        └─────────┬────────┘        └─────────┬────────┘
            │                           │                           │
            │                 ┌─────────▼────────┐                  │
            │                 │  usn::transport  │                  │
            │                 │  (PacketCodec)   │                  │
            │                 └─────────┬────────┘                  │
            │                           │                           │
            └───────────────────┐       │       ┌───────────────────┘
                                │       │       │
                              ┌─▼───────▼───────▼─┐
                              │    usn::model     │
                              │ (Samples, Time)   │
                              └─────────┬─────────┘
                                        │
                              ┌─────────▼─────────┐
                              │   usn::common     │
                              │ (Status, Logging) │
                              └─────────┬─────────┘
                                        │
                              ┌─────────▼─────────┐
                              │   shared/wire     │
                              │ (Single C Header) │
                              └───────────────────┘
```

---

## 3. Core Architectural Rules

### Rule A: The Qt-Free Seam
`usn::app` and `usn::gui` are the only components permitted to reference Qt headers or types (`QObject`, `QString`, etc.). Targets `usn::common`, `usn::model`, `usn::transport`, `usn::trigger`, `usn::hal`, `usn::protocol`, and `usn::core` remain pure C++20. Compliance is validated mechanically via CTest `architecture.qt_free_core`.

### Rule B: Protocol Decoder Isolation
A protocol decoder's entire input is an immutable `SampleWindow` and its output is a list of `DecodedEvent` structures. Decoders never perform I/O, allocate shared threading primitives, or touch hardware sessions.

---

## 4. Threading Model & Concurrency

The capture path utilizes a decoupled producer-consumer pipeline:
1. **Transport / RX Thread:** Reads raw bytes from the link (`ITransport`), feeds them into `PacketCodec`, and pushes whole decoded blocks into an SPSC ingress queue.
2. **Dispatch Thread (`CapturePipeline`):** Pulls blocks from ingress and distributes them across registered lanes according to their assigned `QueuePolicy`:
   - `BlockProducer`: Used for storage and file writes. Blocks producer when full to guarantee zero sample loss.
   - `CoalesceLatest`: Used for the GUI / waveform display. Holds only the most recent block, preventing GUI lag from stalling RX.
   - `DropOldestWithDiagnostic`: Used for real-time analysis. Discards oldest items when backed up and increments drop counters.
3. **GUI Main Thread:** Updates view-models and responds to user interactions using immutable snapshots transferred across the seam.
