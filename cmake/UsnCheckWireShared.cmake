# ----------------------------------------------------------------------------
# UsnCheckWireShared.cmake — run via `cmake -P` as an architecture test.
#
# shared/wire/usn_wire.h is the single source of truth for the USB packet layout
# (docs/architecture_review.md section 8.2, deviation R4). This test guards the
# two properties that make it safe to share with firmware:
#
#   1. it is C-compatible — no C++-only constructs, so arm-none-eabi-gcc can
#      compile it inside the Teensy image;
#   2. the desktop does not define its own private copy of the header layout.
#
# Prints WIRE-SHARED-OK on success.
# ----------------------------------------------------------------------------

if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR must be defined")
endif()

set(_wire_header "${SOURCE_DIR}/shared/wire/usn_wire.h")
if(NOT EXISTS "${_wire_header}")
    message(FATAL_ERROR "missing ${_wire_header}")
endif()

file(READ "${_wire_header}" _raw_content)

# Strip comments before scanning, so that ordinary prose in the documentation
# blocks cannot produce a false positive. CMake regex has no non-greedy match,
# hence the explicit "C comment" pattern below.
string(REGEX REPLACE "/\\*[^*]*\\*+([^/*][^*]*\\*+)*/" "" _content "${_raw_content}")
string(REGEX REPLACE "//[^\n]*" "" _content "${_content}")

# --- 1. C compatibility -----------------------------------------------------
set(_bad_constructs "")
foreach(_pat
        "std::" "template" "namespace" "class " "constexpr" "auto "
        "nullptr" "public:" "private:" "#include <[a-z_]+>")
    if(_content MATCHES "${_pat}")
        list(APPEND _bad_constructs "${_pat}")
    endif()
endforeach()

if(_bad_constructs)
    message(FATAL_ERROR
        "shared/wire/usn_wire.h must remain C-compatible so the Teensy firmware "
        "can include the identical header. Found C++-only constructs: ${_bad_constructs}\n"
        "Use <stdint.h>/<stddef.h>, #define, enum, struct and static_assert only.")
endif()

if(NOT _content MATCHES "USN_WIRE_H")
    message(FATAL_ERROR "usn_wire.h must have an include guard named USN_WIRE_H")
endif()

# --- 2. no private duplicate of the layout ---------------------------------
# What this rule forbids is a SECOND DEFINITION of the layout: a hard-coded magic
# literal, or a local constant that re-states one. It must NOT forbid *using* the
# shared macros -- referencing USN_WIRE_MAGIC_VALUE from the codec is precisely the
# behaviour the rule exists to encourage, and an earlier version of this check
# failed the build for exactly that.
set(_dupes "")
file(GLOB_RECURSE _desktop_srcs
    "${SOURCE_DIR}/desktop/*.h" "${SOURCE_DIR}/desktop/*.hpp" "${SOURCE_DIR}/desktop/*.cpp")
foreach(_f IN LISTS _desktop_srcs)
    file(READ "${_f}" _raw_src)
    # Strip comments first: documentation prose quotes the magic value on purpose.
    string(REGEX REPLACE "/\\*[^*]*\\*+([^/*][^*]*\\*+)*/" "" _src "${_raw_src}")
    string(REGEX REPLACE "//[^\n]*" "" _src "${_src}")

    set(_reason "")
    # A raw magic literal, in either byte order, is a private copy of the layout.
    if(_src MATCHES "0[xX]314[eE]5355" OR _src MATCHES "0[xX]55534[eE]31")
        set(_reason "hard-codes the packet magic literal")
    endif()
    # A definition line mentioning MAGIC is a re-declaration; a plain use is not.
    if(NOT _reason AND _src MATCHES "(#define|constexpr|static[ \t]+const)[^\n]*MAGIC")
        # USN_WIRE_MAGIC_BYTE_n aliases bound to the shared macro are fine, so only
        # flag a definition that does not expand to a USN_WIRE_ symbol.
        string(REGEX MATCH "(#define|constexpr|static[ \t]+const)[^\n]*MAGIC[^\n]*" _def "${_src}")
        if(NOT _def MATCHES "USN_WIRE_MAGIC")
            set(_reason "re-declares a packet magic constant: ${_def}")
        endif()
    endif()
    # Re-stating the header size or the body-length cap is the same violation.
    if(NOT _reason AND _src MATCHES "(#define|constexpr)[^\n]*(HEADER_SIZE|MAX_BODY_LENGTH)[^\n]*=[^\n]*[0-9]")
        string(REGEX MATCH "(#define|constexpr)[^\n]*(HEADER_SIZE|MAX_BODY_LENGTH)[^\n]*" _def2 "${_src}")
        if(NOT _def2 MATCHES "USN_WIRE_")
            set(_reason "re-declares a wire layout constant: ${_def2}")
        endif()
    endif()

    if(_reason)
        string(REPLACE "${SOURCE_DIR}/" "" _rel "${_f}")
        list(APPEND _dupes "${_rel} (${_reason})")
    endif()
endforeach()

if(_dupes)
    message(FATAL_ERROR
        "Packet layout must be defined once, in shared/wire/usn_wire.h.\n"
        "Offending files:\n  ${_dupes}\n"
        "Use the USN_WIRE_* macros from that header instead of restating a value.")
endif()

message("WIRE-SHARED-OK")
