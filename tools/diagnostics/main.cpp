// -----------------------------------------------------------------------------
// diagnostics -- collect system environment and sniffer runtime diagnostics
// into a bundle for troubleshooting and bug reports (master spec section 44).
// -----------------------------------------------------------------------------
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#  define NOMINMAX
#  include <windows.h>
#elif defined(__linux__) || defined(__APPLE__)
#  include <sys/utsname.h>
#  include <unistd.h>
#endif

#include <fmt/core.h>
#include <nlohmann/json.hpp>

#include "usn/common/perf.h"
#include "usn/common/status.h"
#include "usn/core/diagnostics_log.h"
#include "usn/hal/device_manager.h"
#include "usn_wire.h"

namespace {

using json = nlohmann::json;

struct Options {
    std::filesystem::path output;
    bool pretty{false};
    bool verbose{false};
};

void printUsage(const char* prog) {
    fmt::print(stderr,
               "Usage: {} [options]\n\n"
               "Options:\n"
               "  -o, --output <path>    Write diagnostic bundle to file (default: stdout)\n"
               "  -p, --pretty           Pretty-print JSON output\n"
               "  -v, --verbose          Print extra collection details to stderr\n"
               "  -h, --help             Show this help\n",
               prog);
}

bool parseArgs(int argc, char* argv[], Options& opts) {
    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            std::exit(0);
        } else if (arg == "-p" || arg == "--pretty") {
            opts.pretty = true;
        } else if (arg == "-v" || arg == "--verbose") {
            opts.verbose = true;
        } else if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            opts.output = argv[++i];
        } else {
            fmt::print(stderr, "error: unknown argument '{}'\n", arg);
            printUsage(argv[0]);
            return false;
        }
    }
    return true;
}

json collectHostInfo() {
    json info;

#if defined(_WIN32)
    info["os"] = "Windows";
    OSVERSIONINFOEXW osvi{sizeof(osvi)};
    info["kernel"] = "NT";
#elif defined(__linux__)
    info["os"] = "Linux";
    struct utsname uts;
    if (uname(&uts) == 0) {
        info["sysname"] = uts.sysname;
        info["nodename"] = uts.nodename;
        info["release"] = uts.release;
        info["version"] = uts.version;
        info["machine"] = uts.machine;
    }
#elif defined(__APPLE__)
    info["os"] = "macOS";
    struct utsname uts;
    if (uname(&uts) == 0) {
        info["sysname"] = uts.sysname;
        info["release"] = uts.release;
        info["machine"] = uts.machine;
    }
#else
    info["os"] = "Unknown";
#endif

#if defined(__GNUC__)
    info["compiler"] = fmt::format("GCC {}.{}.{}", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#elif defined(_MSC_VER)
    info["compiler"] = fmt::format("MSVC {}", _MSC_VER);
#elif defined(__clang__)
    info["compiler"] = fmt::format("Clang {}.{}.{}", __clang_major__, __clang_minor__, __clang_patchlevel__);
#else
    info["compiler"] = "Unknown";
#endif

#if defined(__cplusplus)
    info["cplusplus"] = __cplusplus;
#endif

    auto now = std::chrono::system_clock::now();
    auto epochSeconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    info["timestampUtc"] = epochSeconds;

    return info;
}

json collectVersionInfo() {
    json v;
    v["product"] = "TooL_For_Sniffer_V1";
    v["version"] = "0.1.0";
    v["wireProtocolVersion"] = USN_WIRE_VERSION;
    v["wireHeaderSize"] = USN_WIRE_HEADER_SIZE;
    v["maxBodyLength"] = USN_WIRE_MAX_BODY_LENGTH;
    return v;
}

json collectDeviceScan(bool verbose) {
    json devInfo = json::array();
    usn::hal::DeviceManager mgr;
    mgr.addProbe(std::make_unique<usn::hal::NullProbe>());

    auto status = mgr.rescan();
    if (verbose) {
        fmt::print(stderr, "diagnostics: probe rescan status: {}\n", status.toString());
    }

    for (const auto& dev : mgr.discovered()) {
        json d;
        d["name"] = dev.name;
        d["endpoint"] = dev.endpoint;
        d["serial"] = dev.serial;
        d["description"] = dev.description;
        devInfo.push_back(std::move(d));
    }
    return devInfo;
}

}  // namespace

int main(int argc, char* argv[]) {
    Options opts;
    if (!parseArgs(argc, argv, opts)) {
        return 1;
    }

    if (opts.verbose) {
        fmt::print(stderr, "diagnostics: collecting bundle...\n");
    }

    json bundle;
    bundle["schemaVersion"] = 1;
    bundle["host"] = collectHostInfo();
    bundle["version"] = collectVersionInfo();
    bundle["devices"] = collectDeviceScan(opts.verbose);

    // Performance registry snapshot
    auto& perf = usn::perf::PerfRegistry::instance();
    try {
        bundle["perf"] = json::parse(perf.toJson());
    } catch (...) {
        bundle["perf"] = json::object();
    }

    std::string outputStr = opts.pretty ? bundle.dump(2) : bundle.dump();

    if (!opts.output.empty()) {
        std::ofstream out(opts.output);
        if (!out.is_open()) {
            fmt::print(stderr, "error: failed to open output file '{}'\n", opts.output.string());
            return 1;
        }
        out << outputStr << "\n";
        if (opts.verbose) {
            fmt::print(stderr, "diagnostics: wrote bundle to '{}'\n", opts.output.string());
        }
    } else {
        std::cout << outputStr << "\n";
    }

    return 0;
}
