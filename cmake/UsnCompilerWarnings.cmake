# ----------------------------------------------------------------------------
# UsnCompilerWarnings.cmake — one interface target, applied to every first-party
# target. Third-party code is consumed through IMPORTED/system targets so its
# headers never trip these flags (master spec section 49: review warnings).
# ----------------------------------------------------------------------------

add_library(usn_warnings INTERFACE)
add_library(usn::warnings ALIAS usn_warnings)

# shared/wire is compiled as C so the Teensy firmware can build the identical
# sources. Several of the C++ warning flags below are rejected by GCC when
# compiling C ("-Wold-style-cast is valid for C++ but not for C"), which under
# -Werror is fatal -- hence a separate, smaller flag set for C targets.
add_library(usn_warnings_c INTERFACE)
add_library(usn::warnings_c ALIAS usn_warnings_c)
if(MSVC)
    target_compile_options(usn_warnings_c INTERFACE /W4 /utf-8 /external:W0)
    if(USN_WARNINGS_AS_ERRORS)
        target_compile_options(usn_warnings_c INTERFACE /WX)
    endif()
else()
    # -Wextra-semi and several others are C++/ObjC++ only; GCC rejects them for C
    # and under -Werror that rejection is fatal.
    target_compile_options(usn_warnings_c INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wcast-align -Wformat=2
        -Wnull-dereference -Wimplicit-fallthrough)
    if(USN_WARNINGS_AS_ERRORS)
        target_compile_options(usn_warnings_c INTERFACE -Werror)
    endif()
endif()

if(MSVC)
    target_compile_options(usn_warnings INTERFACE
        /W4                 # high warning level
        /permissive-        # strict standards conformance
        /Zc:__cplusplus     # report the real __cplusplus value
        /Zc:preprocessor    # conforming preprocessor
        /utf-8              # source and execution charset
        /EHsc               # standard exception model
        /MP                 # parallel compilation
        /external:W0        # silence warnings from system/external headers
        /external:templates-
        /wd4702             # unreachable code: fires inside fmt/gtest headers
    )
    target_compile_definitions(usn_warnings INTERFACE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX              # do not let windows.h define min/max macros
        WIN32_LEAN_AND_MEAN
    )
    if(USN_WARNINGS_AS_ERRORS)
        target_compile_options(usn_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(usn_warnings INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wuseless-cast
        -Wcast-align
        -Wdouble-promotion
        -Wformat=2
        -Wnull-dereference
        -Wimplicit-fallthrough
        -Woverloaded-virtual
        -Wextra-semi
    )
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(usn_warnings INTERFACE -Wduplicated-cond -Wlogical-op)
    endif()
    if(USN_WARNINGS_AS_ERRORS)
        target_compile_options(usn_warnings INTERFACE -Werror)
    endif()
endif()

# Sanitizers live in their own interface target so they apply uniformly to both
# the C++ and the C (shared/wire) targets, and are mutually exclusive.
add_library(usn_sanitizers INTERFACE)
add_library(usn::sanitizers ALIAS usn_sanitizers)

if(USN_ENABLE_SANITIZERS)
    set(_usn_san "address,undefined")
    if(NOT MSVC)
        string(APPEND _usn_san ",leak,pointer-compare,pointer-subtract")
    endif()
    if(MSVC)
        target_compile_options(usn_sanitizers INTERFACE /fsanitize=address)
        target_link_options(usn_sanitizers INTERFACE /INCREMENTAL:NO)
    else()
        target_compile_options(usn_sanitizers INTERFACE
            -fsanitize=${_usn_san} -fno-omit-frame-pointer -fno-sanitize-recover=all)
        target_link_options(usn_sanitizers INTERFACE -fsanitize=${_usn_san})
    endif()
    message(STATUS "Sanitizers enabled: ${_usn_san}")
elseif(USN_ENABLE_TSAN)
    if(MSVC)
        message(FATAL_ERROR "ThreadSanitizer is not supported by MSVC; use GCC or Clang.")
    endif()
    target_compile_options(usn_sanitizers INTERFACE
        -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(usn_sanitizers INTERFACE -fsanitize=thread)
    message(STATUS "ThreadSanitizer enabled")
endif()

target_link_libraries(usn_warnings   INTERFACE usn_sanitizers)
target_link_libraries(usn_warnings_c INTERFACE usn_sanitizers)

# Per-target standard override, e.g. usn_target_cxx_standard(usn_core 23)
function(usn_target_cxx_standard target standard)
    if(NOT standard MATCHES "^(20|23)$")
        message(FATAL_ERROR "usn_target_cxx_standard: standard must be 20 or 23")
    endif()
    target_compile_features(${target} PUBLIC cxx_std_${standard})
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD ${standard}
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
endfunction()

# Convenience: create a first-party static library with warnings and the C++
# standard applied, plus a usn::<name> alias. The alias strips a leading "usn_"
# so target usn_common is reachable as usn::common, matching the names used in
# docs/architecture_review.md section 1.3.
function(usn_add_library target)
    add_library(${target} STATIC)
    set(_alias ${target})
    if(_alias MATCHES "^usn_(.+)$")
        set(_alias "${CMAKE_MATCH_1}")
    endif()
    add_library(usn::${_alias} ALIAS ${target})
    usn_target_cxx_standard(${target} ${USN_CXX_STANDARD})
    target_link_libraries(${target} PRIVATE usn::warnings)
endfunction()
