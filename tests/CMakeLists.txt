# ----------------------------------------------------------------------------
# tests -- the phase gate (master spec section 52).
#
# A test target is registered with a label matching its category so that
# `ctest -L unit`, `-L integration`, `-L fuzz` and `-L golden` each mean something,
# and so the nightly TSan job can run the concurrency-sensitive subset alone.
#
# Tests are registered with add_test(NAME ... COMMAND <binary>) rather than
# gtest_discover_tests(), deliberately: discovery executes the binary at build time,
# which fails when cross-compiling and turns "did my build work?" into "why is my
# build running my tests?". Each binary is one CTest entry; gtest still filters
# inside it.
# ----------------------------------------------------------------------------
include(GoogleTest)

# --- test framework selection ----------------------------------------------
if(USN_TEST_FRAMEWORK STREQUAL "gtest")
    if(NOT TARGET GTest::gtest)
        message(FATAL_ERROR
            "USN_TEST_FRAMEWORK=gtest but GoogleTest is not available. "
            "It is provisioned by FetchContent from codeload.github.com; if that "
            "host is unreachable, configure with -DUSN_TEST_FRAMEWORK=mini to use "
            "the local subset shim instead (see tests/support/mini_gtest).")
    endif()
    set(USN_TEST_LIBS GTest::gtest GTest::gtest_main GTest::gmock)
elseif(USN_TEST_FRAMEWORK STREQUAL "mini")
    message(WARNING
        "USN_TEST_FRAMEWORK=mini: building the test suite against the local "
        "subset shim in tests/support/mini_gtest instead of GoogleTest. Results "
        "from such a run do NOT satisfy the master spec section 52 phase gate on "
        "their own -- re-run with real GoogleTest before sign-off.")
    add_library(usn_mini_gtest STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/support/mini_gtest/mini_gtest_main.cpp)
    target_compile_features(usn_mini_gtest PUBLIC cxx_std_20)
    set_target_properties(usn_mini_gtest PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_include_directories(usn_mini_gtest PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/support/mini_gtest/include)
    target_link_libraries(usn_mini_gtest PUBLIC usn::warnings)
    set(USN_TEST_LIBS usn_mini_gtest)
else()
    message(FATAL_ERROR
        "USN_TEST_FRAMEWORK must be 'gtest' or 'mini', got '${USN_TEST_FRAMEWORK}'")
endif()

# usn_add_test(<name> SOURCES ... LIBS ... LABELS ...)
function(usn_add_test name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBS;LABELS" ${ARGN})
    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "usn_add_test(${name}): SOURCES is required")
    endif()
    if(NOT ARG_LABELS)
        set(ARG_LABELS unit)
    endif()

    add_executable(${name} ${ARG_SOURCES})
    target_compile_features(${name} PRIVATE cxx_std_20)
    set_target_properties(${name} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)

    target_link_libraries(${name} PRIVATE
        ${USN_TEST_LIBS}
        usn::warnings
        ${ARG_LIBS})

    # Keep test binaries out of the install set: they are not deliverables.
    set_target_properties(${name} PROPERTIES EXCLUDE_FROM_ALL FALSE)

    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        LABELS "${ARG_LABELS}"
        TIMEOUT 300)

    # Sanitizer runs abort on the first error; make sure that shows up as a test
    # failure with the report attached rather than as a silent pass.
    if(USN_ENABLE_SANITIZERS OR USN_ENABLE_TSAN)
        set_tests_properties(${name} PROPERTIES
            ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1:abort_on_error=1;TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1")
    endif()
endfunction()

add_subdirectory(support)
add_subdirectory(unit)
add_subdirectory(integration)
add_subdirectory(fuzz)
add_subdirectory(golden)
