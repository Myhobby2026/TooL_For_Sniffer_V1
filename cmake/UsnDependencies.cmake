# ----------------------------------------------------------------------------
# UsnDependencies.cmake — third-party provisioning.
#
# All four dependencies are permissively licensed:
#   {fmt}            BSL-1.0     formatting without pulling Qt into the core
#   nlohmann/json    MIT         config + .usn extensible metadata
#   GoogleTest       BSD-3       unit / component / integration tests
#   Google Benchmark Apache-2.0  micro-benchmarks (opt-in)
#
# Offline builds: FetchContent natively honours FETCHCONTENT_SOURCE_DIR_<NAME>.
# See cmake/UsnOptions.cmake.
# ----------------------------------------------------------------------------

include(FetchContent)

set(USN_DEP_FMT_VERSION       "11.0.2"  CACHE STRING "{fmt} version")
set(USN_DEP_JSON_VERSION      "3.11.3"  CACHE STRING "nlohmann/json version")
set(USN_DEP_GTEST_VERSION     "1.15.2"  CACHE STRING "GoogleTest version")
set(USN_DEP_BENCHMARK_VERSION "1.9.0"   CACHE STRING "Google Benchmark version")

set(USN_DEP_BASE_URL "https://codeload.github.com" CACHE STRING
    "Base URL for dependency archives (override for mirrors/proxies)")

# --- keep third-party builds quiet and out of our install/export sets -------
set(FMT_INSTALL             OFF CACHE BOOL "" FORCE)
set(FMT_DOC                 OFF CACHE BOOL "" FORCE)
set(FMT_TEST                OFF CACHE BOOL "" FORCE)
set(JSON_Install            OFF CACHE BOOL "" FORCE)
set(JSON_BuildTests         OFF CACHE BOOL "" FORCE)
set(JSON_MultipleHeaders    OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST           OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK             ON  CACHE BOOL "" FORCE)
set(gtest_force_shared_crt  ON  CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_TESTING   OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL   OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_WERROR    OFF CACHE BOOL "" FORCE)

# --- VERIFIED CONSTRAINT (docs/architecture_review.md R19 / D6) -------------
# nlohmann/json v3.11.3 declares cmake_minimum_required(VERSION 3.1...3.14).
# CMake >= 4.0 refuses any subproject whose minimum is below 3.5, so configure
# fails without this override. googletest 1.15.2 (3.13), fmt 11.0.2 (3.8...3.28)
# and benchmark 1.9.0 (3.10...3.22) are all fine on CMake 4.x.
# The override is scoped and restored so it cannot leak into other subprojects.
macro(usn_fetch_with_legacy_policy name)
    set(_usn_saved_policy "${CMAKE_POLICY_VERSION_MINIMUM}")
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    FetchContent_MakeAvailable(${name})
    set(CMAKE_POLICY_VERSION_MINIMUM "${_usn_saved_policy}")
endmacro()

function(usn_provide_fmt)
    if(TARGET fmt::fmt)
        return()
    endif()
    if(USN_USE_SYSTEM_DEPS)
        find_package(fmt ${USN_DEP_FMT_VERSION} REQUIRED)
        return()
    endif()
    FetchContent_Declare(fmt
        URL "${USN_DEP_BASE_URL}/fmtlib/fmt/tar.gz/refs/tags/${USN_DEP_FMT_VERSION}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(fmt)
endfunction()

function(usn_provide_json)
    if(TARGET nlohmann_json::nlohmann_json)
        return()
    endif()
    if(USN_USE_SYSTEM_DEPS)
        find_package(nlohmann_json ${USN_DEP_JSON_VERSION} REQUIRED)
        return()
    endif()
    FetchContent_Declare(json
        URL "${USN_DEP_BASE_URL}/nlohmann/json/tar.gz/refs/tags/v${USN_DEP_JSON_VERSION}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    usn_fetch_with_legacy_policy(json)
endfunction()

function(usn_provide_gtest)
    if(TARGET GTest::gtest)
        return()
    endif()
    if(USN_USE_SYSTEM_DEPS)
        find_package(GTest REQUIRED)
        return()
    endif()
    FetchContent_Declare(googletest
        URL "${USN_DEP_BASE_URL}/google/googletest/tar.gz/refs/tags/v${USN_DEP_GTEST_VERSION}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(googletest)
endfunction()

function(usn_provide_benchmark)
    if(TARGET benchmark::benchmark)
        return()
    endif()
    if(USN_USE_SYSTEM_DEPS)
        find_package(benchmark REQUIRED)
        return()
    endif()
    FetchContent_Declare(benchmark
        URL "${USN_DEP_BASE_URL}/google/benchmark/tar.gz/refs/tags/v${USN_DEP_BENCHMARK_VERSION}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(benchmark)
endfunction()

# Core dependencies are always needed (fmt for logging, json for config).
usn_provide_fmt()
usn_provide_json()

if(USN_BUILD_TESTS AND USN_TEST_FRAMEWORK STREQUAL "gtest")
    usn_provide_gtest()
endif()

if(USN_BUILD_BENCHMARKS)
    usn_provide_benchmark()
endif()
