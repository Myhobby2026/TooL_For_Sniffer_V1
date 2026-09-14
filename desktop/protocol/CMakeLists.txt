# usn::protocol -- decoders. Depends on model and common ONLY: no core, no hal,
# no transport, no Qt (Rule B). Verified by tests/unit/architecture_layers_test.
usn_add_library(usn_protocol)

target_sources(usn_protocol PRIVATE
    src/decoder_api.cpp
    src/decoder_registry.cpp)

target_include_directories(usn_protocol PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(usn_protocol
    PUBLIC  usn::model
    PRIVATE fmt::fmt)
