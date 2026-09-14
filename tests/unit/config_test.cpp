// -----------------------------------------------------------------------------
// config_test.cpp -- the configuration system.
//
// The properties that matter: an unknown key is an error (a typo in a saved
// workspace must not silently run with defaults), every declared range is enforced,
// an unsupported version is refused rather than best-effort parsed, and
// serialisation is byte-stable so golden-file testing is possible.
// -----------------------------------------------------------------------------
#include <gtest/gtest.h>

#include <unistd.h>

#include <filesystem>
#include <functional>
#include <fstream>

#include "usn/core/config.h"

using usn::ErrorCode;
using usn::core::AppConfig;
using usn::core::ConfigStore;
using usn::core::ValidationIssue;
using usn::core::kConfigVersion;
using usn::core::limits::kMaxChannels;
using usn::core::validateConfiguration;

namespace {

class TempDir {
public:
    TempDir() : m_path(std::filesystem::temp_directory_path() /
                       ("usn_config_test_" + std::to_string(::getpid()))) {
        std::filesystem::create_directories(m_path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }
    [[nodiscard]] std::filesystem::path path(std::string_view name) const {
        return m_path / name;
    }

private:
    std::filesystem::path m_path;
};

TEST(ConfigTest, DefaultsAreValid) {
    auto defaults = ConfigStore::defaults();
    std::vector<ValidationIssue> issues;
    auto const status = validateConfiguration(defaults, issues);
    for (const auto& issue : issues) {
        ADD_FAILURE() << issue.path << ": " << issue.message;
    }
    EXPECT_TRUE(status.ok());
    EXPECT_EQ(defaults.capture.channelCount, usn::core::limits::kDefaultChannels);
    EXPECT_EQ(defaults.configVersion, kConfigVersion);
}

TEST(ConfigTest, ChannelMaskIsDerivedWhenUnset) {
    auto c = ConfigStore::defaults();
    c.capture.channelMask = 0;
    c.capture.channelCount = 16;
    EXPECT_EQ(c.effectiveChannelMask(), 0xFFFFu);
    c.capture.channelCount = 32;
    EXPECT_EQ(c.effectiveChannelMask(), 0xFFFFFFFFu);
    // An explicit mask is respected even when it is not the low bits.
    c.capture.channelMask = 0xF0F0F0F0u;
    EXPECT_EQ(c.effectiveChannelMask(), 0xF0F0F0F0u);
}

TEST(ConfigTest, MaskDisagreeingWithChannelCountIsRejected) {
    auto c = ConfigStore::defaults();
    c.capture.channelCount = 16;
    c.capture.channelMask = 0xFFu;   // only 8 bits set
    std::vector<ValidationIssue> issues;
    EXPECT_FALSE(validateConfiguration(c, issues).ok());
    ASSERT_FALSE(issues.empty());
    EXPECT_EQ(issues[0].path, "capture.channelMask");
}

TEST(ConfigTest, EveryOutOfRangeValueIsReported) {
    struct Case {
        const char* path;
        std::function<void(AppConfig&)> mutate;
    };
    const std::vector<Case> cases = {
        {"capture.channelCount",
         [](AppConfig& c) { c.capture.channelCount = kMaxChannels + 1; }},
        {"capture.channelCount", [](AppConfig& c) { c.capture.channelCount = 0; }},
        {"capture.strideBytes", [](AppConfig& c) { c.capture.strideBytes = 3; }},
        {"capture.sampleRateHz", [](AppConfig& c) { c.capture.sampleRateHz = 0; }},
        {"capture.blockSamples", [](AppConfig& c) { c.capture.blockSamples = 0; }},
        {"capture.maxSamples", [](AppConfig& c) {
             c.capture.preTriggerSamples = 100;
             c.capture.maxSamples = 10;
         }},
        {"threading.guiLaneCapacity",
         [](AppConfig& c) { c.threading.guiLaneCapacity = 8; }},
        {"threading.ingressBlockTimeoutMs",
         [](AppConfig& c) { c.threading.ingressBlockTimeoutMs = 0; }},
        {"logging.level", [](AppConfig& c) { c.logging.level = "verbose"; }},
        {"logging.ringCapacity", [](AppConfig& c) { c.logging.ringCapacity = 0; }},
        {"storage.fileNamePattern", [](AppConfig& c) { c.storage.fileNamePattern.clear(); }},
        {"storage.chunkBytes", [](AppConfig& c) { c.storage.chunkBytes = 1000; }},
        {"device.preferredTransport",
         [](AppConfig& c) { c.device.preferredTransport = "bluetooth"; }},
        {"device.openTimeoutMs", [](AppConfig& c) { c.device.openTimeoutMs = 0; }},
        {"ui.targetFps", [](AppConfig& c) { c.ui.targetFps = 0; }},
        {"ui.targetFps", [](AppConfig& c) { c.ui.targetFps = 1000; }},
        {"configVersion", [](AppConfig& c) { c.configVersion = kConfigVersion + 1; }},
        {"formatName", [](AppConfig& c) { c.formatName = "someone-elses-config"; }},
    };
    for (const auto& testCase : cases) {
        auto c = ConfigStore::defaults();
        testCase.mutate(c);
        std::vector<ValidationIssue> issues;
        auto const status = validateConfiguration(c, issues);
        EXPECT_FALSE(status.ok()) << testCase.path << " was accepted";
        bool found = false;
        for (const auto& issue : issues) {
            if (issue.path == testCase.path) {
                found = true;
            }
        }
        EXPECT_TRUE(found) << "no issue reported for " << testCase.path;
    }
}

TEST(ConfigTest, BlockSizeIsCheckedAgainstTheWireBodyLimit) {
    auto c = ConfigStore::defaults();
    // 65536 / 2 == 32768 samples would leave no room for the 40-byte block prefix.
    c.capture.strideBytes = 2;
    c.capture.blockSamples = 32'768;
    std::vector<ValidationIssue> issues;
    EXPECT_FALSE(validateConfiguration(c, issues).ok());
    bool found = false;
    for (const auto& issue : issues) {
        found = found || issue.path == "capture.blockSamples";
    }
    EXPECT_TRUE(found);

    // The largest block that still fits: (65536 - 40) / 2.
    c.capture.blockSamples = (65536 - 40) / 2;
    issues.clear();
    EXPECT_TRUE(validateConfiguration(c, issues).ok());
}

TEST(ConfigTest, ValidationCollectsEveryProblemNotJustTheFirst) {
    auto c = ConfigStore::defaults();
    c.capture.strideBytes = 3;
    c.capture.sampleRateHz = 0;
    c.ui.targetFps = 0;
    std::vector<ValidationIssue> issues;
    EXPECT_FALSE(validateConfiguration(c, issues).ok());
    EXPECT_GE(issues.size(), 3u);
}

TEST(ConfigTest, UnknownKeyIsRejectedNotDefaulted) {
    ConfigStore store;
    auto const status = store.loadFromString(R"({
        "configVersion": 1,
        "formatName": "usn-config",
        "capture": { "channelCount": 8 }
    })");
    ASSERT_TRUE(status.ok());

    // A typo. If this were accepted, the user's intended setting would be dropped
    // and the capture would run at 16 channels with no message anywhere.
    auto const typo = store.loadFromString(R"({
        "configVersion": 1,
        "formatName": "usn-config",
        "capture": { "channelCountt": 8 }
    })");
    EXPECT_FALSE(typo.ok());
    EXPECT_EQ(typo.code(), ErrorCode::ConfigurationError);
    EXPECT_NE(std::string(typo.message()).find("channelCountt"), std::string::npos);

    auto const unknownRoot = store.loadFromString(R"({
        "configVersion": 1,
        "formatName": "usn-config",
        "caputre": {}
    })");
    EXPECT_FALSE(unknownRoot.ok());
    EXPECT_NE(std::string(unknownRoot.message()).find("caputre"), std::string::npos);
}

TEST(ConfigTest, UnsupportedVersionIsRefusedNotBestEffortParsed) {
    ConfigStore store;
    auto const status = store.loadFromString(R"({
        "configVersion": 99,
        "formatName": "usn-config",
        "capture": { "channelCount": 8 }
    })");
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), ErrorCode::FileVersionUnsupported);
    // The store must still hold its previous (valid) configuration.
    EXPECT_EQ(store.snapshot().capture.channelCount, usn::core::limits::kDefaultChannels);
}

TEST(ConfigTest, WrongTypesAreRejected) {
    ConfigStore store;
    EXPECT_FALSE(store.loadFromString(R"({"configVersion": 1, "formatName": "usn-config",
                                          "capture": {"channelCount": "eight"}})").ok());
    EXPECT_FALSE(store.loadFromString(R"({"configVersion": 1, "formatName": "usn-config",
                                          "logging": {"console": "yes"}})").ok());
    EXPECT_FALSE(store.loadFromString(R"({"configVersion": 1, "formatName": "usn-config",
                                          "capture": 5})").ok());
    EXPECT_FALSE(store.loadFromString("[1,2,3]").ok());
    EXPECT_FALSE(store.loadFromString("not json at all").ok());
}

TEST(ConfigTest, ValuesOutOfRangeForTheirTypeAreRejected) {
    ConfigStore store;
    // 300 does not fit in the uint8 channelCount field.
    auto const status = store.loadFromString(R"({
        "configVersion": 1, "formatName": "usn-config",
        "capture": { "channelCount": 300 }
    })");
    EXPECT_FALSE(status.ok());
}

TEST(ConfigTest, PartialConfigKeepsDefaultsForOmittedKeys) {
    ConfigStore store;
    ASSERT_TRUE(store.loadFromString(R"({
        "configVersion": 1, "formatName": "usn-config",
        "capture": { "sampleRateHz": 8000000 }
    })").ok());
    auto const c = store.snapshot();
    EXPECT_EQ(c.capture.sampleRateHz, 8'000'000u);
    EXPECT_EQ(c.capture.channelCount, usn::core::limits::kDefaultChannels);
    EXPECT_EQ(c.threading.ingressCapacity, ConfigStore::defaults().threading.ingressCapacity);
    EXPECT_FALSE(store.isDirty());
}

TEST(ConfigTest, CanonicalJsonIsByteStable) {
    ConfigStore a;
    ConfigStore b;
    ASSERT_TRUE(b.loadFromString(a.toCanonicalJson()).ok());
    // Round-tripping through text must not change a single byte. Without this, a
    // golden file would differ on every save and the test would be meaningless.
    EXPECT_EQ(a.toCanonicalJson(), b.toCanonicalJson());

    // Independent of the order keys appear in the input.
    ConfigStore c;
    ASSERT_TRUE(c.loadFromString(R"({
        "ui": {"theme": "dark"},
        "capture": {"channelCount": 16},
        "configVersion": 1,
        "formatName": "usn-config"
    })").ok());
    ConfigStore d;
    ASSERT_TRUE(d.loadFromString(R"({
        "configVersion": 1,
        "formatName": "usn-config",
        "capture": {"channelCount": 16},
        "ui": {"theme": "dark"}
    })").ok());
    EXPECT_EQ(c.toCanonicalJson(), d.toCanonicalJson());
}

TEST(ConfigTest, UpdateValidatesAndLeavesTheStoreUntouchedOnFailure) {
    ConfigStore store;
    auto bad = store.snapshot();
    bad.capture.strideBytes = 3;
    EXPECT_FALSE(store.update(bad).ok());
    EXPECT_EQ(store.snapshot().capture.strideBytes, 2);

    auto good = store.snapshot();
    good.capture.channelCount = 8;
    good.capture.channelMask = 0xFF;
    ASSERT_TRUE(store.update(good).ok());
    EXPECT_EQ(store.snapshot().capture.channelCount, 8);
    EXPECT_TRUE(store.isDirty());
}

TEST(ConfigTest, ListenerIsNotifiedOnlyOnSuccessfulChange) {
    ConfigStore store;
    int notifications = 0;
    std::uint8_t lastChannels = 0;
    store.addListener([&](const AppConfig& c) {
        notifications += 1;
        lastChannels = c.capture.channelCount;
    });
    auto bad = store.snapshot();
    bad.ui.targetFps = 0;
    EXPECT_FALSE(store.update(bad).ok());
    EXPECT_EQ(notifications, 0);

    auto good = store.snapshot();
    good.capture.channelCount = 4;
    good.capture.channelMask = 0xF;
    ASSERT_TRUE(store.update(good).ok());
    EXPECT_EQ(notifications, 1);
    EXPECT_EQ(lastChannels, 4);
}

TEST(ConfigTest, FileRoundTrip) {
    TempDir tmp;
    auto const path = tmp.path("roundtrip.json");
    ConfigStore writer;
    auto config = writer.snapshot();
    config.capture.channelCount = 8;
    config.capture.channelMask = 0xFF;
    config.capture.sampleRateHz = 4'000'000;
    ASSERT_TRUE(writer.update(config).ok());
    ASSERT_TRUE(writer.saveToFile(path).ok());
    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_FALSE(writer.isDirty());
    EXPECT_EQ(writer.sourcePath(), path);

    ConfigStore reader;
    ASSERT_TRUE(reader.loadFromFile(path).ok());
    auto const loaded = reader.snapshot();
    EXPECT_EQ(loaded.capture.channelCount, 8);
    EXPECT_EQ(loaded.capture.sampleRateHz, 4'000'000u);
    EXPECT_EQ(reader.toCanonicalJson(), writer.toCanonicalJson());
}

TEST(ConfigTest, MissingFileIsAnErrorNotSilentDefaults) {
    TempDir tmp;
    ConfigStore store;
    auto const status = store.loadFromFile(tmp.path("does_not_exist.json"));
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), ErrorCode::FileOpenFailed);
}

TEST(ConfigTest, LoadOrDefaultsCreatesAnEditableFile) {
    TempDir tmp;
    auto const path = tmp.path("nested") / "settings.json";
    ConfigStore store;
    ASSERT_TRUE(store.loadOrDefaults(path).ok());
    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_EQ(store.sourcePath(), path);
    EXPECT_FALSE(store.isDirty());

    // Second call loads what was written, and the result must be identical.
    ConfigStore second;
    ASSERT_TRUE(second.loadOrDefaults(path).ok());
    EXPECT_EQ(second.toCanonicalJson(), store.toCanonicalJson());
}

TEST(ConfigTest, LogLevelParsing) {
    EXPECT_EQ(*usn::core::parseLogLevel("trace"), usn::log::Level::Trace);
    EXPECT_EQ(*usn::core::parseLogLevel("warning"), usn::log::Level::Warning);
    EXPECT_EQ(*usn::core::parseLogLevel("warn"), usn::log::Level::Warning);
    EXPECT_EQ(*usn::core::parseLogLevel("off"), usn::log::Level::Off);
    EXPECT_FALSE(usn::core::parseLogLevel("chatty").ok());
    EXPECT_FALSE(usn::core::parseLogLevel("").ok());
}

TEST(ConfigTest, BoolParsing) {
    for (std::string_view text : {"true", "1", "yes", "on"}) {
        auto const value = usn::core::parseBool(text);
        ASSERT_TRUE(value.ok()) << text;
        EXPECT_TRUE(*value) << text;
    }
    for (std::string_view text : {"false", "0", "no", "off"}) {
        auto const value = usn::core::parseBool(text);
        ASSERT_TRUE(value.ok()) << text;
        EXPECT_FALSE(*value) << text;
    }
    EXPECT_FALSE(usn::core::parseBool("maybe").ok());
}

TEST(ConfigTest, SustainedRateIsDerivedNotStored) {
    auto c = ConfigStore::defaults();
    c.capture.sampleRateHz = 10'000'000;
    c.capture.strideBytes = 2;
    // 10 MSPS at 2 bytes per sample == 20 MB/s of packed payload.
    EXPECT_EQ(c.sustainedBytesPerSecond(), 20'000'000u);
}

TEST(ConfigTest, ApplyLoggingRejectsAnUnopenableFile) {
    ConfigStore store;
    auto c = store.snapshot();
    c.logging.filePath = "/proc/1/definitely/not/writable/usn.log";
    ASSERT_TRUE(store.update(c).ok());
    EXPECT_FALSE(store.applyLogging().ok());
}

}  // namespace
