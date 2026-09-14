# ----------------------------------------------------------------------------
# UsnCheckQtFree.cmake — run via `cmake -P` as an architecture test.
#
# Fails if any source file in a Qt-free layer includes a Qt header, or links Qt
# via a QT macro. Prints QT-FREE-CORE-OK on success (matched by
# PASS_REGULAR_EXPRESSION in UsnLayerCheck.cmake).
# ----------------------------------------------------------------------------

if(NOT DEFINED SOURCE_DIR OR NOT DEFINED LAYERS)
    message(FATAL_ERROR "SOURCE_DIR and LAYERS must be defined")
endif()

set(_violations "")
set(_files_scanned 0)

foreach(_layer IN LISTS LAYERS)
    set(_dir "${SOURCE_DIR}/${_layer}")
    if(NOT EXISTS "${_dir}")
        continue()
    endif()
    file(GLOB_RECURSE _srcs
        "${_dir}/*.h" "${_dir}/*.hpp" "${_dir}/*.cpp" "${_dir}/*.cc" "${_dir}/*.cxx")
    foreach(_f IN LISTS _srcs)
        math(EXPR _files_scanned "${_files_scanned} + 1")
        file(READ "${_f}" _content)
        # Qt headers are <Q...> or <Qt.../...>; also catch qobject/qml macro use.
        if(_content MATCHES "#[ \t]*include[ \t]*[<\"]Q[A-Za-z]"
           OR _content MATCHES "#[ \t]*include[ \t]*[<\"]Qt[A-Z]"
           OR _content MATCHES "#[ \t]*include[ \t]*[<\"]q[a-z]+\\.h[>\"]"
           OR _content MATCHES "Q_OBJECT"
           OR _content MATCHES "Q_PROPERTY"
           OR _content MATCHES "QML_ELEMENT")
            string(REPLACE "${SOURCE_DIR}/" "" _rel "${_f}")
            list(APPEND _violations "${_layer}: ${_rel}")
        endif()
    endforeach()
endforeach()

if(_violations)
    message(FATAL_ERROR
        "Rule A violated — Qt is not permitted below desktop/app.\n"
        "Scanned ${_files_scanned} files. Offending files:\n  "
        "${_violations}\n"
        "See docs/architecture_review.md section 1.2. Move Qt-dependent code to "
        "desktop/app or desktop/gui.")
endif()

message(STATUS "Scanned ${_files_scanned} files across layers: ${LAYERS}")
message("QT-FREE-CORE-OK")
