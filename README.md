# TooL_For_Sniffer_V1 — Universal Sniffer

A professional universal logic analyzer / digital signal sniffer / protocol analyzer / bus monitor
platform.

**Target stack:** C++20 · Qt 6 / QML · CMake + Ninja · MSVC · Teensy 4.1 (NXP i.MX RT1062) · Windows 10/11 64-bit, portable to Linux/macOS.

---

## Project status

| Phase | Status |
|---|---|
| Architecture review | ✅ **Complete — awaiting owner approval** |
| Phase 1 — Foundation | ⏸ Not started (gated on review approval) |
| Phases 2–18 | ⏸ Not started |

**No production code exists in this repository yet.** This is deliberate: the project specification
requires an architecture review as the first deliverable, and requires that each phase build, pass its
tests, and have documented interfaces before the next phase begins.

## Documentation

- **[docs/architecture_review.md](docs/architecture_review.md)** — the current primary deliverable.
  Covers the recommended architecture, repository tree, module responsibilities, C++ interface
  proposals, Qt/QML architecture, thread model, Teensy firmware architecture, the USB wire protocol,
  timestamp strategy, capture data model, decoder API, trigger architecture, the `.usn` file format,
  testing architecture, performance measurement strategy, verified hardware limitations, risks, and the
  Phase 1 implementation plan.
  - §0.1 states explicitly which claims were verified and how, versus which are proposed design.
  - §19 records every deviation from the master specification with rationale, benefits, risks, and a
    testing plan.
  - §20 lists the open decisions that require an owner answer before Phase 1 proceeds.

The remaining documents (`architecture.md`, `capture_protocol.md`, `file_format.md`, `decoder_api.md`,
`trigger_engine.md`, `performance.md`, `development.md`, `hardware_constraints.md`) are Phase 1
deliverables and do not exist yet.

## Headline engineering constraint

Read this before expecting any particular sample rate:

> Sustained capture rate on Teensy 4.1 is bounded by **USB payload throughput**, and burst depth is
> bounded by **on-chip RAM** (≈512 KB OCRAM; PSRAM is optional solder-on hardware). The GPIO sampler is
> not the bottleneck. `480 Mbit/s` is a USB *link* rate, not a payload rate — the realistic planning
> envelope is **5–20 MB/s, to be measured**, which at 4 bytes per sample (≤32 channels) is
> **1.25–5 MSPS continuous**.

No sample-rate figure will appear in this project's UI or documentation until it is backed by a
committed measurement record on named hardware. See `docs/architecture_review.md` §16.

## Licence

To be added in Phase 1 (P1.1).
