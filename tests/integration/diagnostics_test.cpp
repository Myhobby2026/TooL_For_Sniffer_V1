// -----------------------------------------------------------------------------
// diagnostics_test.cpp -- integration test executing the diagnostics CLI tool.
// -----------------------------------------------------------------------------
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#ifndef USN_DIAGNOSTICS_PATH
#  error "USN_DIAGNOSTICS_PATH must be defined"
#endif

namespace {

using json = nlohmann::json;

}  // namespace

TEST(DiagnosticsToolTest, RunsAndProducesValidJsonBundle) {
    auto tempFile = std::filesystem::temp_directory_path() / "usn_test_diag_bundle.json";
    if (std::filesystem::exists(tempFile)) {
        std::filesystem::remove(tempFile);
    }

    std::string cmd = std::string(USN_DIAGNOSTICS_PATH) + " -o \"" + tempFile.string() + "\" -p";
    int ret = std::system(cmd.c_str());
    EXPECT_EQ(ret, 0);

    ASSERT_TRUE(std::filesystem::exists(tempFile));
    std::ifstream in(tempFile);
    ASSERT_TRUE(in.is_open());

    json bundle;
    ASSERT_NO_THROW(in >> bundle);

    EXPECT_EQ(bundle["schemaVersion"], 1);
    EXPECT_TRUE(bundle.contains("host"));
    EXPECT_TRUE(bundle["host"].contains("os"));
    EXPECT_TRUE(bundle["host"].contains("compiler"));
    EXPECT_TRUE(bundle.contains("version"));
    EXPECT_EQ(bundle["version"]["product"], "TooL_For_Sniffer_V1");
    EXPECT_EQ(bundle["version"]["version"], "0.1.0");
    EXPECT_TRUE(bundle.contains("devices"));
    EXPECT_TRUE(bundle.contains("perf"));

    std::filesystem::remove(tempFile);
}
