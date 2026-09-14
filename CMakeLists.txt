cmake_minimum_required(VERSION 3.21...3.30)

# ----------------------------------------------------------------------------
# Universal Sniffer
#
# Layering rule (docs/architecture_review.md section 1.2, "Rule A"):
# nothing below desktop/app may depend on Qt. USN_ENABLE_GUI therefore
# auto-disables when Qt 6 is absent, so the engine and its whole test suite
# remain buildable headless on any toolchain.
# ----------------------------------------------------------------------------

project(UniversalSniffer
    VERSION 0.1.0
    DESCRIPTION "Universal logic analyzer / protocol analyzer platform"
    HOMEPAGE_URL "https://github.com/Myhobby2026/TooL_For_Sniffer_V1"
    LANGUAGES CXX C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(UsnOptions)
include(UsnCompilerWarnings)
include(UsnDependencies)

# --- global build settings -------------------------------------------------
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo)
endif()

# Single output tree so tools/tests find each other without install steps.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# --- sub-projects ----------------------------------------------------------
add_subdirectory(shared/wire)
add_subdirectory(desktop)

if(USN_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(USN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(USN_ENABLE_GUI)
    include(UsnQt)
    usn_setup_qt()
    add_subdirectory(desktop/app)
    add_subdirectory(desktop/gui)
endif()

# --- configuration summary -------------------------------------------------
include(UsnLayerCheck)

message(STATUS "")
message(STATUS "================ Universal Sniffer ================")
message(STATUS "  Version            : ${PROJECT_VERSION}")
message(STATUS "  C++ standard       : ${USN_CXX_STANDARD}")
message(STATUS "  Build type         : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler           : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Warnings as errors : ${USN_WARNINGS_AS_ERRORS}")
message(STATUS "  Sanitizers         : ${USN_SANITIZERS}")
message(STATUS "  Tests              : ${USN_BUILD_TESTS}")
message(STATUS "  Tools              : ${USN_BUILD_TOOLS}")
message(STATUS "  GUI (Qt 6)         : ${USN_ENABLE_GUI}")
if(NOT USN_ENABLE_GUI)
    message(STATUS "    -> disabled (${USN_GUI_DISABLED_REASON})")
endif()
message(STATUS "===================================================")
message(STATUS "")
