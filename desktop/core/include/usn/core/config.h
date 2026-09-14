// -----------------------------------------------------------------------------
// config.h -- the configuration system (master spec section 51, Phase 1 item).
//
// Rules this file exists to enforce:
//
//   1. Unknown keys are errors, not defaults. A saved workspace with a typo must
//      fail loudly rather than quietly run with something the user did not ask for.
//   2. Every value has a declared range, and loading validates against it. There is
//      no path by which an out-of-range value reaches a device.
//   3. Serialisation is canonical: keys are emitted in a fixed order with fixed
//      indentation, so two logically identical configs produce byte-identical files.
//      That is what makes golden-file testing of config possible at all.
//   4. The config is versioned, and a version the loader does not understand is
//      rejected rather than best-effort parsed.
//   5. It is Qt-free. The GUI reads and writes AppConfig; it does not own it.
// -----------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "usn/common/log.h"
#include "usn/common/status.h"

namespace usn::core {

inline constexpr std::uint32_t kConfigVersion = 1;
inline constexpr std::string_view kConfigFormatName = "usn-config";

// Bounds are declared here and used by both the loader and validate(), so the
// validation rules cannot drift between the two.
namespace limits {
inline constexpr std::uint8_t kMinChannels = 1;
inline constexpr std::uint8_t kMaxChannels = 32;
inline constexpr std::uint8_t kDefaultChannels = 16;
inline constexpr std::uint64_t kMinSampleRateHz = 1;
inline constexpr std::uint64_t kMaxSampleRateHz = 100'000'000;
inline constexpr std::uint64_t kDefaultSampleRateHz = 1'000'000;
inline constexpr std::size_t kMinQueueCapacity = 1;
inline constexpr std::size_t kMaxQueueCapacity = 1u << 20;
inline constexpr std::uint32_t kMaxBlockTimeoutMs = 60'000;
inline constexpr std::uint32_t kMaxFlushIntervalMs = 60'000;
inline constexpr std::uint64_t kMaxPreTriggerSamples = 1ull << 40;
}  // namespace limits

struct LoggingConfig {
    // One of trace/debug/info/warning/error/critical/off.
    std::string level{"info"};
    bool console{true};
    std::string filePath;              // empty = no file sink
    std::uint32_t ringCapacity{4096};  // in-memory ring for the diagnostics panel
    bool includeTimestamp{true};
    bool includeThread{true};
};

struct CaptureConfig {
    std::uint8_t channelCount{limits::kDefaultChannels};
    std::uint8_t strideBytes{2};
    std::uint64_t sampleRateHz{limits::kDefaultSampleRateHz};
    std::uint64_t channelMask{0};      // 0 = "derive from channelCount"
    std::uint64_t preTriggerSamples{0};
    std::uint64_t maxSamples{0};       // 0 = unbounded (stop on user request)
    std::uint32_t blockSamples{8192};  // samples per wire block
    bool rleEnabled{false};            // specified now, implemented in Phase 3
};

struct ThreadingConfig {
    std::size_t ingressCapacity{512};
    std::uint32_t ingressBlockTimeoutMs{2000};
    std::size_t storageLaneCapacity{256};
    std::size_t guiLaneCapacity{1};    // always 1: the GUI coalesces
    std::size_t decodeLaneCapacity{128};
    bool enablePerfProbes{false};
};

struct StorageConfig {
    std::string directory;             // empty = process working directory
    std::string fileNamePattern{"capture_%Y%m%d_%H%M%S.usn"};
    bool autoFlush{true};
    std::uint32_t flushIntervalMs{1000};
    std::uint64_t chunkBytes{4ull << 20};   // master spec: 4 MiB chunks
};

struct DeviceConfig {
    std::string preferredTransport{"cdc"};  // cdc first; vendor-bulk added in Phase 2
    std::string serialFilter;               // empty = accept any
    std::uint16_t vendorId{0};              // 0 = do not filter on VID
    std::uint16_t productId{0};             // 0 = do not filter on PID
    std::uint32_t openTimeoutMs{3000};
};

struct UiConfig {
    std::string theme{"dark"};
    std::string language{"en"};
    std::uint32_t targetFps{60};
    bool showDiagnosticsPanel{true};
    // Presented as "not characterised" until the device answers; never a fake
    // number, so the default is explicitly zero.
    std::uint64_t displayedMaxRateHz{0};
};

struct AppConfig {
    std::uint32_t configVersion{kConfigVersion};
    std::string formatName{std::string(kConfigFormatName)};
    LoggingConfig logging;
    CaptureConfig capture;
    ThreadingConfig threading;
    StorageConfig storage;
    DeviceConfig device;
    UiConfig ui;

    // Derives a channel mask from channelCount when the stored mask is 0, so the
    // two can never disagree in practice.
    [[nodiscard]] std::uint64_t effectiveChannelMask() const noexcept;
    // Bytes of packed sample data produced per second at this configuration.
    [[nodiscard]] std::uint64_t sustainedBytesPerSecond() const noexcept;
};

// Validation collects every problem instead of stopping at the first one: a user
// fixing a config file should not have to reload five times.
struct ValidationIssue {
    std::string path;      // JSON-pointer-ish, e.g. "capture.strideBytes"
    std::string message;
};

[[nodiscard]] Status validateConfiguration(const AppConfig& config,
                                          std::vector<ValidationIssue>& issues);

class ConfigStore {
public:
    ConfigStore();
    ~ConfigStore();

    ConfigStore(const ConfigStore&) = delete;
    ConfigStore& operator=(const ConfigStore&) = delete;

    [[nodiscard]] static AppConfig defaults();

    [[nodiscard]] Status loadFromFile(const std::filesystem::path& path);
    [[nodiscard]] Status loadFromString(std::string_view jsonText,
                                        const std::filesystem::path& origin = {});
    [[nodiscard]] Status saveToFile(const std::filesystem::path& path) const;

    // Byte-stable text. Used for golden files and for the .usn META section.
    [[nodiscard]] std::string toCanonicalJson() const;

    [[nodiscard]] AppConfig snapshot() const;
    // Validates first; the stored config is left untouched on failure.
    [[nodiscard]] Status update(AppConfig config);

    [[nodiscard]] std::filesystem::path sourcePath() const;
    [[nodiscard]] bool isDirty() const;      // changed since load/save
    void markClean();

    using ChangeListener = std::function<void(const AppConfig&)>;
    void addListener(ChangeListener listener);

    // Applies logging.* to the process logger. Explicit rather than automatic:
    // a config load should not silently change log verbosity in the middle of a
    // capture unless the caller asks for it.
    [[nodiscard]] Status applyLogging() const;

    // Convenience for main(): load from path if it exists, otherwise write defaults
    // there so the user has a file to edit.
    [[nodiscard]] Status loadOrDefaults(const std::filesystem::path& path);

private:
    mutable std::mutex m_mutex;
    AppConfig m_config;
    // Mutable because saveToFile() is logically const with respect to the config
    // itself while still recording where it came from.
    mutable std::filesystem::path m_source;
    mutable bool m_dirty{false};
    std::vector<ChangeListener> m_listeners;
};

// Parsed from a string so config files stay human-editable. Unknown spellings are
// rejected by the caller rather than defaulting.
[[nodiscard]] StatusOr<bool> parseBool(std::string_view text);
[[nodiscard]] StatusOr<log::Level> parseLogLevel(std::string_view text);
[[nodiscard]] std::string_view logLevelName(log::Level level) noexcept;

}  // namespace usn::core
