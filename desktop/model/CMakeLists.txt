# usn::model -- pure value types. No I/O, no threads, no Qt, no protocol
# knowledge. Exists as its own layer because core and protocol both need
# DecodedEvent/SampleBlock; without it those two would form a dependency cycle
# (docs/architecture_review.md section 19, deviation D1).
usn_add_library(usn_model)

target_sources(usn_model PRIVATE
    src/time.cpp
    src/sample.cpp
    src/channel.cpp
    src/capabilities.cpp
    src/events.cpp
    src/diagnostics.cpp)

target_include_directories(usn_model PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(usn_model
    PUBLIC  usn::common
    PRIVATE fmt::fmt)
