# Shared test fixtures. This is a library, not a test target, so fixtures cannot
# drift between suites the way copy-pasted helpers would.
usn_add_library(usn_test_support)

target_sources(usn_test_support PRIVATE
    test_fixtures.cpp)

target_include_directories(usn_test_support PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(usn_test_support
    PUBLIC  usn::wire usn::core usn::hal usn::transport usn::model usn::trigger usn::protocol
    PRIVATE fmt::fmt)
