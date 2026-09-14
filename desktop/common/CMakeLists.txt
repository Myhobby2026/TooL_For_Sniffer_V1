# usn::common -- the foundation layer. Nothing above it; no Qt (Rule A).
usn_add_library(usn_common)

target_sources(usn_common PRIVATE
    src/status.cpp
    src/log.cpp
    src/perf.cpp)

target_include_directories(usn_common PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

# PUBLIC: Crc32c in crc32c.h wraps the single shared C implementation, and
# log.h formats through fmt in a public macro, so both propagate to consumers.
target_link_libraries(usn_common PUBLIC usn::wire fmt::fmt)
