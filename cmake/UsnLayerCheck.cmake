# ----------------------------------------------------------------------------
# UsnLayerCheck.cmake — mechanical enforcement of Rule A.
#
# Rule A: nothing in common / model / transport / hal / protocol / trigger / core
# may include a Qt header. Without this, "the core is Qt-free" is an aspiration
# rather than a property, and headless CI quietly stops working.
#
# Implemented as a CTest so it fails the build like any other test.
# ----------------------------------------------------------------------------

set(USN_QT_FREE_LAYERS
    common model transport hal protocol trigger core)

if(USN_BUILD_TESTS)
    add_test(
        NAME architecture.qt_free_core
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/desktop
            -DLAYERS=${USN_QT_FREE_LAYERS}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/UsnCheckQtFree.cmake)
    set_tests_properties(architecture.qt_free_core PROPERTIES
        LABELS "architecture;unit"
        PASS_REGULAR_EXPRESSION "QT-FREE-CORE-OK")

    add_test(
        NAME architecture.wire_header_shared
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/UsnCheckWireShared.cmake)
    set_tests_properties(architecture.wire_header_shared PROPERTIES
        LABELS "architecture;unit"
        PASS_REGULAR_EXPRESSION "WIRE-SHARED-OK")
endif()
