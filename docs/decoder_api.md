# Universal Sniffer — Protocol Decoder API Specification

**Status:** Current (Phase 1 Baseline)  
**Header:** `desktop/protocol/include/usn/protocol/decoder_api.h`  
**Registry:** `desktop/protocol/include/usn/protocol/decoder_registry.h`

---

## 1. Design Philosophy

Protocol decoders in Universal Sniffer operate as deterministic, pure transformation functions over packed sample slices:
- **Input:** `SampleWindow` (borrowed view of packed samples).
- **Output:** Vector of `DecodedEvent` (atomic protocol elements) and `Transaction` (aggregate high-level transfers).
- **Zero I/O:** Decoders do not read files, make system calls, or communicate over networks.
- **Independence:** Decoders do not depend on Qt, GUI structures, or hardware sessions.

---

## 2. Core Interfaces

### `IProtocolDecoder`

Every decoder implements `IProtocolDecoder`:

```cpp
namespace usn::protocol {

class IProtocolDecoder {
public:
    virtual ~IProtocolDecoder() = default;

    [[nodiscard]] virtual DecoderInfo info() const = 0;
    [[nodiscard]] virtual Status configure(const DecoderConfiguration& config) = 0;
    [[nodiscard]] virtual DecoderConfiguration configuration() const = 0;

    virtual void reset() = 0;

    // Process a borrowed window of packed samples
    [[nodiscard]] virtual Status process(const SampleWindow& window) = 0;

    // Drain produced atomic events
    [[nodiscard]] virtual std::vector<DecodedEvent> takeEvents() = 0;

    // Optional: drain aggregated transactions
    [[nodiscard]] virtual std::vector<Transaction> takeTransactions() { return {}; }

    // Checkpoint support for viewport/lazy decoding
    [[nodiscard]] virtual DecoderCheckpoint saveCheckpoint() const = 0;
    [[nodiscard]] virtual Status restoreCheckpoint(const DecoderCheckpoint& checkpoint) = 0;

    [[nodiscard]] virtual std::uint64_t samplesProcessed() const noexcept = 0;
};

} // namespace usn::protocol
```

---

## 3. Configuration & Parameter Descriptors

Decoders expose configuration parameters generically via `ConfigParameterDescriptor` (e.g. Baud Rate, CPOL, CPHA, CS Polarity). The UI renders these dynamically without protocol-specific GUI code.

### Channel Roles
Decoders request channels by role:
- `Clock`, `Tx`, `Rx`, `Data0`, `Data1`, `Sda`, `Scl`, `ChipSelect`, `Frame`

---

## 4. Checkpoints & Lazy Decoding

To support interactive scrolling across gigabyte-scale captures, decoders serialize internal state via `saveCheckpoint()` and restore it via `restoreCheckpoint()`. This allows decoding viewport ranges on-demand from the nearest checkpoint without reprocessing the entire capture from sample 0.
