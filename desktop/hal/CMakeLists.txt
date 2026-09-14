# usn::hal -- device abstractions. Everything above depends on these interfaces
# rather than on a Teensy-specific implementation (master spec section 9).
# FakeCaptureDevice ships here, not in tests/, because tools and replay use it.
usn_add_library(usn_hal)

target_sources(usn_hal PRIVATE
    src/idevice.cpp
    src/device_manager.cpp
    src/fake_capture_device.cpp)

target_include_directories(usn_hal PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(usn_hal
    PUBLIC  usn::transport usn::trigger
    PRIVATE fmt::fmt)
