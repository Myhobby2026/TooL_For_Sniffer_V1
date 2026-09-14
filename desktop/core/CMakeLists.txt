# ----------------------------------------------------------------------------
# usn::core -- capture orchestration, integrity and configuration.
#
# Layer rule: depends on common / model / trigger / hal / protocol, and on nothing
# above it. It must stay Qt-free. Rule A is enforced mechanically by the
# architecture.qt_free_core CTest registered in cmake/UsnLayerCheck.cmake, which
# already lists "core" among the scanned layers; duplicating the scan here would
# create two places to keep in sync.
# ----------------------------------------------------------------------------
usn_add_library(usn_core)

target_sources(usn_core PRIVATE
    src/timeline.cpp
    src/diagnostics_log.cpp
    src/capture_pipeline.cpp
    src/istorage.cpp
    src/config.cpp
    src/capture_session.cpp)

target_include_directories(usn_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(usn_core
    PUBLIC  usn::common usn::model usn::trigger usn::hal usn::protocol
    PRIVATE fmt::fmt nlohmann_json::nlohmann_json)
