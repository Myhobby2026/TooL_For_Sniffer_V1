# usn::trigger -- trigger AST, classification, JSON round-trip.
# The device compiler and the host evaluator are Phase 11; the AST and the
# capability classification are Phase 1 because ITriggerEngine depends on them.
usn_add_library(usn_trigger)

target_sources(usn_trigger PRIVATE src/trigger_node.cpp)

target_include_directories(usn_trigger PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(usn_trigger
    PUBLIC  usn::model
    PRIVATE fmt::fmt nlohmann_json::nlohmann_json)
