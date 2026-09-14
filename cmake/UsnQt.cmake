# ----------------------------------------------------------------------------
# UsnQt.cmake — Qt 6 setup, only reached when USN_ENABLE_GUI is ON.
#
# Rule A (docs/architecture_review.md section 1.2): this file may be included
# ONLY from the top level after desktop/ has been added, and only desktop/app
# and desktop/gui may link the targets it produces.
# ----------------------------------------------------------------------------

function(usn_setup_qt)
    find_package(Qt6 ${USN_QT_MIN_VERSION} REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2)

    set(CMAKE_AUTOMOC ON PARENT_SCOPE)
    set(CMAKE_AUTORCC ON PARENT_SCOPE)

    # Deterministic QML cache + module output so builds are reproducible.
    set(QT_QML_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/qml-modules" CACHE PATH "" FORCE)

    message(STATUS "Qt ${Qt6_VERSION} found — GUI enabled")
    if(Qt6_VERSION VERSION_LESS "6.8")
        message(WARNING
            "Qt ${Qt6_VERSION} is below the recommended 6.8 LTS. "
            "qt_add_qml_module and RHI behaviour may differ from CI.")
    endif()
endfunction()

# Create a QML module target bound to an existing C++ target.
function(usn_add_qml_module target)
    cmake_parse_arguments(ARG "" "URI;VERSION" "QML_FILES;SOURCES;RESOURCES" ${ARGN})
    if(NOT ARG_URI)
        message(FATAL_ERROR "usn_add_qml_module: URI is required for ${target}")
    endif()
    if(NOT ARG_VERSION)
        set(ARG_VERSION "1.0")
    endif()
    qt_add_qml_module(${target}
        URI ${ARG_URI}
        VERSION ${ARG_VERSION}
        QML_FILES ${ARG_QML_FILES}
        SOURCES ${ARG_SOURCES}
        RESOURCES ${ARG_RESOURCES}
        NO_PLUGIN)
endfunction()
