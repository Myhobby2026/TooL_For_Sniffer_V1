# ----------------------------------------------------------------------------
# shared/wire -- the USB packet format, defined ONCE for both sides.
#
# Compiled as C11 so the Teensy firmware can build the identical sources with
# arm-none-eabi-gcc. The desktop gets the same object code, which is what makes
# firmware/host protocol drift a compile-time failure rather than a data
# corruption bug (docs/architecture_review.md sections 8.2, 19/R4).
#
# Firmware integration: add these two .c files to the Teensy build and put this
# directory on its include path. Do NOT copy or fork them.
# ----------------------------------------------------------------------------

add_library(usn_wire STATIC
    usn_wire_crc.c
    usn_wire_rle.c)
add_library(usn::wire ALIAS usn_wire)

# SYSTEM is deliberate and load-bearing. These headers must stay C-compatible so
# the firmware can compile them, and C has no static_cast -- so they necessarily
# contain C-style casts that C++ -Wold-style-cast/-Wuseless-cast reject. Marking
# the directory SYSTEM is the GCC/Clang analogue of MSVC /external:W0 and treats
# shared/wire as what it is from the desktop's point of view: an external,
# separately-verified interface.
#
# The headers are NOT warning-exempt overall: usn_wire_rle.c includes them and is
# compiled as C with -Wall -Wextra -Wpedantic -Werror via usn::warnings_c, and the
# static_asserts on every struct size and field offset are evaluated by both
# toolchains regardless of warning suppression.
target_include_directories(usn_wire SYSTEM PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include/usn/wire>)

target_link_libraries(usn_wire PRIVATE usn::warnings_c)

set_target_properties(usn_wire PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME "usn_wire")

# The C-compatibility of usn_wire.h (no C++-only constructs, so the firmware can
# include the identical file) is enforced by the CTest
# 'architecture.wire_header_shared', registered in cmake/UsnLayerCheck.cmake.
