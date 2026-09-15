# Universal Sniffer — Trigger Engine Specification

**Status:** Current (Phase 1 Baseline)  
**Header:** `desktop/trigger/include/usn/trigger/trigger_node.h`

---

## 1. Principles

1. **Triggers Are Data:** A trigger is represented as an Abstract Syntax Tree (`TriggerNode`), serialized as JSON.
2. **Two Compilation Targets:**
   - **Device Subset:** Executed directly in firmware/hardware (Edge, Level, Simple Pattern).
   - **Host Evaluator:** Evaluates complex multi-stage combinators, protocol field matches, pulse-width timing, and search expressions over stored samples.
3. **Pre-Arm Validation & Classification:** The engine inspects every AST node against the reported `DeviceCapabilities` using `classifyTree()`. If any node cannot be executed on hardware, the UI explicitly flags whether it is `DeviceAndHost` or `HostOnly`.

---

## 2. AST Node Types

- **Leaves:** `Edge` (Rising, Falling, Any), `Level` (High, Low), `Pattern` (bitmask match), `PulseWidth` (min/max duration), `Timeout`, `ProtocolField`, `SearchExpr`.
- **Combinators:** `And`, `Or`, `Not`, `Sequence`, `Count`.
- **Meta:** `Always`, `Never`.
