# ----------------------------------------------------------------------------
# UsnOptions.cmake — project options and capability probes.
#
# C++ standard floor is C++20 (docs/architecture_review.md section 19, D4):
# std::expected needs GCC 13 / MSVC 19.36, and the CI matrix includes GCC 12.
# A target may opt into 23 via usn_target_cxx_standard().
# ----------------------------------------------------------------------------

set(USN_CXX_STANDARD 20 CACHE STRING "C++ standard floor (20 or 23)")
set_property(CACHE USN_CXX_STANDARD PROPERTY STRINGS 20 23)
if(NOT USN_CXX_STANDARD MATCHES "^(20|23)$")
    message(FATAL_ERROR "USN_CXX_STANDARD must be 20 or 23 (got '${USN_CXX_STANDARD}')")
endif()

option(USN_BUILD_TESTS           "Build the test suite"                 ON)
option(USN_BUILD_TOOLS           "Build command-line tools"             ON)
option(USN_BUILD_BENCHMARKS      "Build Google Benchmark targets"       OFF)
option(USN_WARNINGS_AS_ERRORS    "Treat compiler warnings as errors"    ON)
option(USN_ENABLE_GUI            "Build the Qt 6 / QML application"     ON)
option(USN_ENABLE_SANITIZERS     "Enable address+UB sanitizers"         OFF)
option(USN_ENABLE_TSAN           "Enable thread sanitizer"              OFF)
option(USN_USE_SYSTEM_DEPS       "find_package() instead of FetchContent" OFF)

set(USN_QT_MIN_VERSION "6.8" CACHE STRING "Minimum Qt 6 version")

# Test framework selection. "gtest" (the default, and what the master spec
# requires) provisions GoogleTest via FetchContent. "mini" substitutes the local
# subset shim in tests/support/mini_gtest for environments where GoogleTest cannot
# be downloaded; every binary built that way prints a banner saying so, and a run
# against the shim does NOT satisfy the section 52 phase gate on its own.
set(USN_TEST_FRAMEWORK "gtest" CACHE STRING "Test framework: gtest | mini")
set_property(CACHE USN_TEST_FRAMEWORK PROPERTY STRINGS gtest mini)

# --- Qt availability probe --------------------------------------------------
# Rule A means the engine never needs Qt, so a missing Qt 6 must degrade the
# build to "core + tests" with a clear message rather than fail outright.
if(USN_ENABLE_GUI)
    set(USN_GUI_DISABLED_REASON "enabled")
else()
    set(USN_GUI_DISABLED_REASON "disabled explicitly via -DUSN_ENABLE_GUI=OFF")
endif()
if(USN_ENABLE_GUI)
    find_package(Qt6 ${USN_QT_MIN_VERSION} QUIET COMPONENTS Core Gui Qml Quick)
    if(NOT Qt6_FOUND)
        find_package(Qt6 6.5 QUIET COMPONENTS Core Gui Qml Quick)
        if(Qt6_FOUND)
            message(WARNING
                "Qt ${Qt6_VERSION} found but ${USN_QT_MIN_VERSION} is recommended. "
                "Proceeding; QML module and RHI behaviour may differ.")
        else()
            set(USN_ENABLE_GUI OFF CACHE BOOL "Build the Qt 6 / QML application" FORCE)
            set(USN_GUI_DISABLED_REASON
                "Qt6 >= ${USN_QT_MIN_VERSION} (Core/Gui/Qml/Quick) not found; core-only build")
        endif()
    endif()
endif()

if(USN_ENABLE_GUI AND USN_ENABLE_SANITIZERS)
    message(WARNING "ASan + Qt Quick can produce false positives in the QML JIT; "
                    "prefer the 'asan' preset on a core-only build.")
endif()

# --- sanitizer conflict guard ----------------------------------------------
if(USN_ENABLE_SANITIZERS AND USN_ENABLE_TSAN)
    message(FATAL_ERROR
        "USN_ENABLE_SANITIZERS (ASan/UBSan) and USN_ENABLE_TSAN are mutually exclusive. "
        "Use separate build directories: presets 'asan' and 'tsan'.")
endif()

# --- dependency source override (offline / restricted CI) -------------------
# FetchContent honours FETCHCONTENT_SOURCE_DIR_<NAME> natively. Documented here
# because it is the supported way to build without network access:
#   cmake -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/path/to/googletest ...
foreach(_dep fmt json googletest benchmark)
    string(TOUPPER "${_dep}" _DEP)
    if(FETCHCONTENT_SOURCE_DIR_${_DEP})
        message(STATUS "Using pre-populated source for ${_dep}: "
                       "${FETCHCONTENT_SOURCE_DIR_${_DEP}}")
    endif()
endforeach()
