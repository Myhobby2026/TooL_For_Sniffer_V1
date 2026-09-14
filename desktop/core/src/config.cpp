// -----------------------------------------------------------------------------
// config.cpp -- see config.h.
// -----------------------------------------------------------------------------
#include "usn/core/config.h"

#include <algorithm>
#include <array>
#include <bit>
#include <fstream>
#include <sstream>
#include <utility>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "usn/common/log.h"

namespace usn::core {

namespace {

using Json = nlohmann::json;

// Declared once per object so the "no unknown keys" rule is a table rather than a
// hand-maintained if-chain that a newly added field forgets to join.
constexpr std::array<const char*, 6> kLoggingFields = {
    "level", "console", "filePath", "ringCapacity", "includeTimestamp", "includeThread"};
constexpr std::array<const char*, 8> kCaptureFields = {"channelCount", "strideBytes",
                                                       "sampleRateHz", "channelMask",
                                                       "preTriggerSamples", "maxSamples",
                                                       "blockSamples",   "rleEnabled"};
constexpr std::array<const char*, 6> kThreadingFields = {"ingressCapacity",
                                                         "ingressBlockTimeoutMs",
                                                         "storageLaneCapacity",
                                                         "guiLaneCapacity",
                                                         "decodeLaneCapacity",
                                                         "enablePerfProbes"};
constexpr std::array<const char*, 5> kStorageFields = {"directory", "fileNamePattern", "autoFlush",
                                                       "flushIntervalMs", "chunkBytes"};
constexpr std::array<const char*, 5> kDeviceFields = {"preferredTransport", "serialFilter",
                                                      "vendorId", "productId", "openTimeoutMs"};
constexpr std::array<const char*, 5> kUiFields = {"theme", "language", "targetFps",
                                                  "showDiagnosticsPanel", "displayedMaxRateHz"};
constexpr std::array<const char*, 8> kRootFields = {"configVersion", "formatName",
                                                    "logging",   "capture",
                                                    "threading", "storage",
                                                    "device",    "ui"};

template <std::size_t N>
bool isKnownN(const Json& obj, const std::array<const char*, N>& allowed, std::string& badKey) {
    for (const auto& [key, value] : obj.items()) {
        (void)value;
        if (std::find(std::begin(allowed), std::end(allowed), key) == std::end(allowed)) {
            badKey = key;
            return false;
        }
    }
    return true;
}

// Canonical emission: sorted keys, two-space indent, no trailing whitespace.
// nlohmann's ordered_map preserves insertion order, so we build with a std::map to
// get deterministic key order regardless of the order the structs were filled in.
Json toJsonOrdered(const AppConfig& c) {
    Json j;
    j["configVersion"] = c.configVersion;
    j["formatName"] = c.formatName;

    j["capture"] = Json{
        {"blockSamples", c.capture.blockSamples},
        {"channelCount", c.capture.channelCount},
        {"channelMask", c.capture.channelMask},
        {"maxSamples", c.capture.maxSamples},
        {"preTriggerSamples", c.capture.preTriggerSamples},
        {"rleEnabled", c.capture.rleEnabled},
        {"sampleRateHz", c.capture.sampleRateHz},
        {"strideBytes", c.capture.strideBytes},
    };
    j["device"] = Json{
        {"openTimeoutMs", c.device.openTimeoutMs},
        {"preferredTransport", c.device.preferredTransport},
        {"productId", c.device.productId},
        {"serialFilter", c.device.serialFilter},
        {"vendorId", c.device.vendorId},
    };
    j["logging"] = Json{
        {"console", c.logging.console},
        {"filePath", c.logging.filePath},
        {"includeThread", c.logging.includeThread},
        {"includeTimestamp", c.logging.includeTimestamp},
        {"level", c.logging.level},
        {"ringCapacity", c.logging.ringCapacity},
    };
    j["storage"] = Json{
        {"autoFlush", c.storage.autoFlush},
        {"chunkBytes", c.storage.chunkBytes},
        {"directory", c.storage.directory},
        {"fileNamePattern", c.storage.fileNamePattern},
        {"flushIntervalMs", c.storage.flushIntervalMs},
    };
    j["threading"] = Json{
        {"decodeLaneCapacity", c.threading.decodeLaneCapacity},
        {"enablePerfProbes", c.threading.enablePerfProbes},
        {"guiLaneCapacity", c.threading.guiLaneCapacity},
        {"ingressBlockTimeoutMs", c.threading.ingressBlockTimeoutMs},
        {"ingressCapacity", c.threading.ingressCapacity},
        {"storageLaneCapacity", c.threading.storageLaneCapacity},
    };
    j["ui"] = Json{
        {"displayedMaxRateHz", c.ui.displayedMaxRateHz},
        {"language", c.ui.language},
        {"showDiagnosticsPanel", c.ui.showDiagnosticsPanel},
        {"targetFps", c.ui.targetFps},
        {"theme", c.ui.theme},
    };
    return j;
}

template <typename T>
bool getNumber(const Json& obj, const char* key, T& out) {
    auto it = obj.find(key);
    if (it == obj.end()) {
        return true;   // absent: keep the default
    }
    if (!it->is_number_integer() && !it->is_number_unsigned()) {
        return false;
    }
    try {
        out = it->get<T>();
    } catch (const Json::exception&) {
        return false;  // out of range for T
    }
    return true;
}

bool getString(const Json& obj, const char* key, std::string& out) {
    auto it = obj.find(key);
    if (it == obj.end()) {
        return true;
    }
    if (!it->is_string()) {
        return false;
    }
    out = it->get<std::string>();
    return true;
}

bool getBool(const Json& obj, const char* key, bool& out) {
    auto it = obj.find(key);
    if (it == obj.end()) {
        return true;
    }
    if (!it->is_boolean()) {
        return false;
    }
    out = it->get<bool>();
    return true;
}

Status parseSection(const Json& root, AppConfig& out) {
    if (!root.is_object()) {
        return Status::error(ErrorCode::ConfigurationError, "config root is not an object");
    }
    std::string badKey;
    if (!isKnownN(root, kRootFields, badKey)) {
        return Status::error(ErrorCode::ConfigurationError,
                             fmt::format("unknown top-level config key '{}'", badKey));
    }

    if (auto it = root.find("configVersion"); it != root.end()) {
        if (!it->is_number_unsigned() && !it->is_number_integer()) {
            return Status::error(ErrorCode::ConfigurationError, "configVersion is not an integer");
        }
        auto const version = it->get<std::uint32_t>();
        if (version != kConfigVersion) {
            // A best-effort parse of a newer file could silently drop a setting the
            // user depends on. Refuse and say which versions are understood.
            return Status::error(ErrorCode::FileVersionUnsupported,
                                 fmt::format("config version {} is not supported (this build "
                                             "understands version {})",
                                             version, kConfigVersion));
        }
        out.configVersion = version;
    }
    if (!getString(root, "formatName", out.formatName)) {
        return Status::error(ErrorCode::ConfigurationError, "formatName must be a string");
    }
    if (out.formatName != kConfigFormatName) {
        return Status::error(ErrorCode::FileFormatUnsupported,
                             fmt::format("formatName '{}' is not '{}'", out.formatName,
                                         kConfigFormatName));
    }

    if (auto it = root.find("logging"); it != root.end()) {
        if (!it->is_object()) {
            return Status::error(ErrorCode::ConfigurationError, "'logging' must be an object");
        }
        if (!isKnownN(*it, kLoggingFields, badKey)) {
            return Status::error(ErrorCode::ConfigurationError,
                                 fmt::format("unknown key 'logging.{}'", badKey));
        }
        if (!getString(*it, "level", out.logging.level) ||
            !getBool(*it, "console", out.logging.console) ||
            !getString(*it, "filePath", out.logging.filePath) ||
            !getNumber(*it, "ringCapacity", out.logging.ringCapacity) ||
            !getBool(*it, "includeTimestamp", out.logging.includeTimestamp) ||
            !getBool(*it, "includeThread", out.logging.includeThread)) {
            return Status::error(ErrorCode::ConfigurationError,
                                 "a 'logging' value has the wrong type");
        }
    }
    if (auto it = root.find("capture"); it != root.end()) {
        if (!it->is_object()) {
            return Status::error(ErrorCode::ConfigurationError, "'capture' must be an object");
        }
        if (!isKnownN(*it, kCaptureFields, badKey)) {
            return Status::error(ErrorCode::ConfigurationError,
                                 fmt::format("unknown key 'capture.{}'", badKey));
        }
        if (!getNumber(*it, "channelCount", out.capture.channelCount) ||
            !getNumber(*it, "strideBytes", out.capture.strideBytes) ||
            !getNumber(*it, "sampleRateHz", out.capture.sampleRateHz) ||
            !getNumber(*it, "channelMask", out.capture.channelMask) ||
            !getNumber(*it, "preTriggerSamples", out.capture.preTriggerSamples) ||
            !getNumber(*it, "maxSamples", out.capture.maxSamples) ||
            !getNumber(*it, "blockSamples", out.capture.blockSamples) ||
            !getBool(*it, "rleEnabled", out.capture.rleEnabled)) {
            return Status::error(ErrorCode::ConfigurationError,
                                 "a 'capture' value has the wrong type");
        }
    }
    if (auto it = root.find("threading"); it != root.end()) {
        if (!it->is_object()) {
            return Status::error(ErrorCode::ConfigurationError, "'threading' must be an object");
        }
        if (!isKnownN(*it, kThreadingFields, badKey)) {
            return Status::error(ErrorCode::ConfigurationError,
                                 fmt::format("unknown key 'threading.{}'", badKey));
        }
        if (!getNumber(*it, "ingressCapacity", out.threading.ingressCapacity) ||
            !getNumber(*it, "ingressBlockTimeoutMs", out.threading.ingressBlockTimeoutMs) ||
            !getNumber(*it, "storageLaneCapacity", out.threading.storageLaneCapacity) ||
            !getNumber(*it, "guiLaneCapacity", out.threading.guiLaneCapacity) ||
            !getNumber(*it, "decodeLaneCapacity", out.threading.decodeLaneCapacity) ||
            !getBool(*it, "enablePerfProbes", out.threading.enablePerfProbes)) {
            return Status::error(ErrorCode::ConfigurationError,
                                 "a 'threading' value has the wrong type");
        }
    }
    if (auto it = root.find("storage"); it != root.end()) {
        if (!it->is_object()) {
            return Status::error(ErrorCode::ConfigurationError, "'storage' must be an object");
        }
        if (!isKnownN(*it, kStorageFields, badKey)) {
            return Status::error(ErrorCode::ConfigurationError,
                                 fmt::format("unknown key 'storage.{}'", badKey));
        }
        if (!getString(*it, "directory", out.storage.directory) ||
            !getString(*it, "fileNamePattern", out.storage.fileNamePattern) ||
            !getBool(*it, "autoFlush", out.storage.autoFlush) ||
            !getNumber(*it, "flushIntervalMs", out.storage.flushIntervalMs) ||
            !getNumber(*it, "chunkBytes", out.storage.chunkBytes)) {
            return Status::error(ErrorCode::ConfigurationError,
                                 "a 'storage' value has the wrong type");
        }
    }
    if (auto it = root.find("device"); it != root.end()) {
        if (!it->is_object()) {
            return Status::error(ErrorCode::ConfigurationError, "'device' must be an object");
        }
        if (!isKnownN(*it, kDeviceFields, badKey)) {
            return Status::error(ErrorCode::ConfigurationError,
                                 fmt::format("unknown key 'device.{}'", badKey));
        }
        if (!getString(*it, "preferredTransport", out.device.preferredTransport) ||
            !getString(*it, "serialFilter", out.device.serialFilter) ||
            !getNumber(*it, "vendorId", out.device.vendorId) ||
            !getNumber(*it, "productId", out.device.productId) ||
            !getNumber(*it, "openTimeoutMs", out.device.openTimeoutMs)) {
            return Status::error(ErrorCode::ConfigurationError,
                                 "a 'device' value has the wrong type");
        }
    }
    if (auto it = root.find("ui"); it != root.end()) {
        if (!it->is_object()) {
            return Status::error(ErrorCode::ConfigurationError, "'ui' must be an object");
        }
        if (!isKnownN(*it, kUiFields, badKey)) {
            return Status::error(ErrorCode::ConfigurationError,
                                 fmt::format("unknown key 'ui.{}'", badKey));
        }
        if (!getString(*it, "theme", out.ui.theme) ||
            !getString(*it, "language", out.ui.language) ||
            !getNumber(*it, "targetFps", out.ui.targetFps) ||
            !getBool(*it, "showDiagnosticsPanel", out.ui.showDiagnosticsPanel) ||
            !getNumber(*it, "displayedMaxRateHz", out.ui.displayedMaxRateHz)) {
            return Status::error(ErrorCode::ConfigurationError,
                                 "a 'ui' value has the wrong type");
        }
    }
    return Status::success();
}

}  // namespace

std::uint64_t AppConfig::effectiveChannelMask() const noexcept {
    if (capture.channelMask != 0) {
        return capture.channelMask;
    }
    if (capture.channelCount == 0) {
        return 0;
    }
    if (capture.channelCount >= 64) {
        return ~std::uint64_t{0};
    }
    return (std::uint64_t{1} << capture.channelCount) - 1;
}

std::uint64_t AppConfig::sustainedBytesPerSecond() const noexcept {
    return capture.sampleRateHz * static_cast<std::uint64_t>(capture.strideBytes);
}

Status validateConfiguration(const AppConfig& config, std::vector<ValidationIssue>& issues) {
    issues.clear();
    auto const add = [&issues](std::string path, std::string message) {
        issues.push_back(ValidationIssue{std::move(path), std::move(message)});
    };

    if (config.configVersion != kConfigVersion) {
        add("configVersion", fmt::format("{} is not the supported version {}",
                                         config.configVersion, kConfigVersion));
    }
    if (config.formatName != kConfigFormatName) {
        add("formatName", fmt::format("'{}' is not '{}'", config.formatName, kConfigFormatName));
    }

    // -- capture -------------------------------------------------------------
    if (config.capture.channelCount < limits::kMinChannels ||
        config.capture.channelCount > limits::kMaxChannels) {
        add("capture.channelCount",
            fmt::format("{} is outside {}..{}", config.capture.channelCount, limits::kMinChannels,
                        limits::kMaxChannels));
    }
    if (config.capture.strideBytes != 1 && config.capture.strideBytes != 2 &&
        config.capture.strideBytes != 4) {
        add("capture.strideBytes",
            fmt::format("{} is not 1, 2 or 4", config.capture.strideBytes));
    }
    if (config.capture.sampleRateHz < limits::kMinSampleRateHz ||
        config.capture.sampleRateHz > limits::kMaxSampleRateHz) {
        add("capture.sampleRateHz",
            fmt::format("{} Hz is outside {}..{} Hz", config.capture.sampleRateHz,
                        limits::kMinSampleRateHz, limits::kMaxSampleRateHz));
    }
    if (config.capture.channelMask != 0 &&
        std::popcount(config.capture.channelMask) !=
            static_cast<int>(config.capture.channelCount)) {
        add("capture.channelMask",
            fmt::format("mask 0x{:x} has {} bits set but channelCount is {}",
                        config.capture.channelMask, std::popcount(config.capture.channelMask),
                        config.capture.channelCount));
    }
    if (config.capture.preTriggerSamples > limits::kMaxPreTriggerSamples) {
        add("capture.preTriggerSamples",
            fmt::format("{} exceeds the supported maximum {}", config.capture.preTriggerSamples,
                        limits::kMaxPreTriggerSamples));
    }
    if (config.capture.maxSamples != 0 &&
        config.capture.maxSamples < config.capture.preTriggerSamples) {
        add("capture.maxSamples",
            "maxSamples is smaller than preTriggerSamples, so the capture could never complete");
    }
    if (config.capture.blockSamples == 0) {
        add("capture.blockSamples", "must be greater than zero");
    } else {
        auto const blockBytes = static_cast<std::uint64_t>(config.capture.blockSamples) *
                                static_cast<std::uint64_t>(config.capture.strideBytes);
        // The wire format caps bodyLength at 65536, and the SAMPLE_BLOCK prefix takes
        // 40 of those bytes. Exceeding it cannot be framed at all.
        if (blockBytes + 40 > 65536) {
            add("capture.blockSamples",
                fmt::format("{} samples x {} bytes = {} payload bytes, which with the 40-byte "
                            "block prefix exceeds the 65536-byte wire body limit",
                            config.capture.blockSamples, config.capture.strideBytes, blockBytes));
        }
    }

    // -- threading -----------------------------------------------------------
    auto const checkCapacity = [&add](const char* path, std::size_t value) {
        if (value < limits::kMinQueueCapacity || value > limits::kMaxQueueCapacity) {
            add(path, fmt::format("{} is outside {}..{}", value, limits::kMinQueueCapacity,
                                  limits::kMaxQueueCapacity));
        }
    };
    checkCapacity("threading.ingressCapacity", config.threading.ingressCapacity);
    checkCapacity("threading.storageLaneCapacity", config.threading.storageLaneCapacity);
    checkCapacity("threading.decodeLaneCapacity", config.threading.decodeLaneCapacity);
    if (config.threading.guiLaneCapacity != 1) {
        add("threading.guiLaneCapacity",
            fmt::format("{} must be 1: the GUI lane coalesces to the latest snapshot so it can "
                        "never stall the capture path",
                        config.threading.guiLaneCapacity));
    }
    if (config.threading.ingressBlockTimeoutMs == 0 ||
        config.threading.ingressBlockTimeoutMs > limits::kMaxBlockTimeoutMs) {
        add("threading.ingressBlockTimeoutMs",
            fmt::format("{} ms is outside 1..{} ms", config.threading.ingressBlockTimeoutMs,
                        limits::kMaxBlockTimeoutMs));
    }

    // -- logging -------------------------------------------------------------
    if (!parseLogLevel(config.logging.level).ok()) {
        add("logging.level", fmt::format("'{}' is not a recognised level", config.logging.level));
    }
    if (config.logging.ringCapacity == 0) {
        add("logging.ringCapacity", "must be greater than zero");
    }

    // -- storage -------------------------------------------------------------
    if (config.storage.fileNamePattern.empty()) {
        add("storage.fileNamePattern", "must not be empty");
    }
    if (config.storage.flushIntervalMs > limits::kMaxFlushIntervalMs) {
        add("storage.flushIntervalMs",
            fmt::format("{} ms exceeds {} ms", config.storage.flushIntervalMs,
                        limits::kMaxFlushIntervalMs));
    }
    if (config.storage.chunkBytes == 0 || (config.storage.chunkBytes % 4096) != 0) {
        add("storage.chunkBytes",
            fmt::format("{} must be a non-zero multiple of 4096", config.storage.chunkBytes));
    }

    // -- device --------------------------------------------------------------
    static constexpr std::array<const char*, 3> kTransports = {"cdc", "vendor-bulk", "file"};
    if (std::find(std::begin(kTransports), std::end(kTransports),
                  config.device.preferredTransport) == std::end(kTransports)) {
        add("device.preferredTransport",
            fmt::format("'{}' is not one of cdc/vendor-bulk/file",
                        config.device.preferredTransport));
    }
    if (config.device.openTimeoutMs == 0) {
        add("device.openTimeoutMs", "must be greater than zero");
    }

    // -- ui ------------------------------------------------------------------
    if (config.ui.targetFps == 0 || config.ui.targetFps > 240) {
        add("ui.targetFps", fmt::format("{} is outside 1..240", config.ui.targetFps));
    }

    if (!issues.empty()) {
        std::string joined;
        for (const auto& issue : issues) {
            if (!joined.empty()) {
                joined += "; ";
            }
            joined += issue.path + ": " + issue.message;
        }
        return Status::error(ErrorCode::ConfigurationError,
                             fmt::format("{} configuration problem(s): {}", issues.size(), joined));
    }
    return Status::success();
}

ConfigStore::ConfigStore() : m_config(defaults()) {}
ConfigStore::~ConfigStore() = default;

AppConfig ConfigStore::defaults() {
    AppConfig c;
    c.capture.channelMask = 0;   // derived from channelCount by effectiveChannelMask()
    return c;
}

Status ConfigStore::loadFromString(std::string_view const jsonText,
                                   const std::filesystem::path& origin) {
    Json root;
    try {
        root = Json::parse(jsonText);
    } catch (const Json::exception& e) {
        return Status::error(ErrorCode::FileCorrupt,
                             fmt::format("config is not valid JSON: {}", e.what()));
    }
    AppConfig candidate = defaults();
    auto const parsed = parseSection(root, candidate);
    if (!parsed.ok()) {
        return parsed;
    }
    std::vector<ValidationIssue> issues;
    auto const validity = validateConfiguration(candidate, issues);
    if (!validity.ok()) {
        for (const auto& issue : issues) {
            USN_LOG_ERROR(log::cats::kApp, "config {} -- {}", issue.path, issue.message);
        }
        return validity;
    }
    std::vector<ChangeListener> listeners;
    {
        std::scoped_lock const lock(m_mutex);
        m_config = candidate;
        m_source = origin;
        m_dirty = false;
        listeners = m_listeners;
    }
    for (const auto& listener : listeners) {
        listener(candidate);
    }
    USN_LOG_INFO(log::cats::kApp,
                 "config loaded: {} ch @ {} Hz, stride {} B, transport '{}'{}",
                 candidate.capture.channelCount, candidate.capture.sampleRateHz,
                 candidate.capture.strideBytes, candidate.device.preferredTransport,
                 origin.empty() ? std::string() : fmt::format(" from {}", origin.string()));
    return Status::success();
}

Status ConfigStore::loadFromFile(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return Status::error(ErrorCode::FileOpenFailed,
                             fmt::format("config file '{}' does not exist", path.string()));
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return Status::error(ErrorCode::FileOpenFailed,
                             fmt::format("could not open config file '{}'", path.string()));
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return loadFromString(buffer.str(), path);
}

Status ConfigStore::loadOrDefaults(const std::filesystem::path& path) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        return loadFromFile(path);
    }
    auto const written = saveToFile(path);
    if (!written.ok()) {
        return written;
    }
    std::scoped_lock const lock(m_mutex);
    m_source = path;
    m_dirty = false;
    USN_LOG_INFO(log::cats::kApp, "no config at '{}'; wrote defaults", path.string());
    return Status::success();
}

std::string ConfigStore::toCanonicalJson() const {
    AppConfig local;
    {
        std::scoped_lock const lock(m_mutex);
        local = m_config;
    }
    return toJsonOrdered(local).dump(2);
}

Status ConfigStore::saveToFile(const std::filesystem::path& path) const {
    auto const text = toCanonicalJson();
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        // create_directories on an existing directory is not an error, so only a
        // real ec is reported.
        if (ec) {
            return Status::error(ErrorCode::FileWriteFailed,
                                 fmt::format("could not create '{}': {}",
                                             path.parent_path().string(), ec.message()));
        }
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return Status::error(ErrorCode::FileWriteFailed,
                             fmt::format("could not open '{}' for writing", path.string()));
    }
    out << text << '\n';
    out.flush();
    if (!out) {
        return Status::error(ErrorCode::FileWriteFailed,
                             fmt::format("write to '{}' failed", path.string()));
    }
    std::scoped_lock const lock(m_mutex);
    m_source = path;
    m_dirty = false;
    return Status::success();
}

AppConfig ConfigStore::snapshot() const {
    std::scoped_lock const lock(m_mutex);
    return m_config;
}

Status ConfigStore::update(AppConfig config) {
    std::vector<ValidationIssue> issues;
    auto const validity = validateConfiguration(config, issues);
    if (!validity.ok()) {
        for (const auto& issue : issues) {
            USN_LOG_ERROR(log::cats::kApp, "config {} -- {}", issue.path, issue.message);
        }
        return validity;   // the stored config is untouched
    }
    std::vector<ChangeListener> listeners;
    {
        std::scoped_lock const lock(m_mutex);
        m_config = config;
        m_dirty = true;
        listeners = m_listeners;
    }
    for (const auto& listener : listeners) {
        listener(config);
    }
    return Status::success();
}

std::filesystem::path ConfigStore::sourcePath() const {
    std::scoped_lock const lock(m_mutex);
    return m_source;
}

bool ConfigStore::isDirty() const {
    std::scoped_lock const lock(m_mutex);
    return m_dirty;
}

void ConfigStore::markClean() {
    std::scoped_lock const lock(m_mutex);
    m_dirty = false;
}

void ConfigStore::addListener(ChangeListener listener) {
    if (!listener) {
        return;
    }
    std::scoped_lock const lock(m_mutex);
    m_listeners.push_back(std::move(listener));
}

Status ConfigStore::applyLogging() const {
    AppConfig local;
    {
        std::scoped_lock const lock(m_mutex);
        local = m_config;
    }
    auto const level = parseLogLevel(local.logging.level);
    if (!level.ok()) {
        return level.status();
    }
    auto& logger = log::Logger::instance();
    logger.setLevelForAll(*level);

    std::vector<std::shared_ptr<log::ISink>> sinks;
    if (local.logging.console) {
        sinks.push_back(std::make_shared<log::ConsoleSink>());
    }
    if (!local.logging.filePath.empty()) {
        auto fileSink = std::make_shared<log::FileSink>(local.logging.filePath);
        if (!fileSink->isOpen()) {
            return Status::error(ErrorCode::FileOpenFailed,
                                 fmt::format("could not open log file '{}'",
                                             local.logging.filePath));
        }
        sinks.push_back(std::move(fileSink));
    }
    if (local.logging.ringCapacity > 0) {
        sinks.push_back(std::make_shared<log::RingBufferSink>(local.logging.ringCapacity));
    }
    logger.removeAllSinks();
    for (auto& sink : sinks) {
        logger.addSink(std::move(sink));
    }
    return Status::success();
}

StatusOr<bool> parseBool(std::string_view const text) {
    if (text == "true" || text == "1" || text == "yes" || text == "on") {
        return true;
    }
    if (text == "false" || text == "0" || text == "no" || text == "off") {
        return false;
    }
    return Status::error(ErrorCode::ConfigurationError,
                         fmt::format("'{}' is not a boolean", text));
}

StatusOr<log::Level> parseLogLevel(std::string_view const text) {
    static constexpr std::array<std::pair<std::string_view, log::Level>, 7> kLevels = {{
        {"trace", log::Level::Trace},     {"debug", log::Level::Debug},
        {"info", log::Level::Info},       {"warning", log::Level::Warning},
        {"warn", log::Level::Warning},    {"error", log::Level::Error},
        {"critical", log::Level::Critical},
    }};
    for (const auto& [name, level] : kLevels) {
        if (text == name) {
            return level;
        }
    }
    if (text == "off" || text == "none") {
        return log::Level::Off;
    }
    return Status::error(ErrorCode::ConfigurationError,
                         fmt::format("'{}' is not a log level", text));
}

std::string_view logLevelName(log::Level const level) noexcept { return log::nameOf(level); }

}  // namespace usn::core
