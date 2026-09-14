# usn::transport -- bytes on a link. Knows nothing about what the bytes mean.
usn_add_library(usn_transport)

target_sources(usn_transport PRIVATE
    src/packet_codec.cpp
    src/loopback_transport.cpp
    src/file_transport.cpp
    src/transport_manager.cpp)

target_include_directories(usn_transport PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(usn_transport
    PUBLIC  usn::model
    PRIVATE fmt::fmt)
