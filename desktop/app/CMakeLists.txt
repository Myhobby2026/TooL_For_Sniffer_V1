add_library(usn_app STATIC
    src/application.cpp
    src/qt_sink.cpp
    src/diagnostics_model.cpp
    src/channel_model.cpp
    src/device_model.cpp
    src/capture_view_model.cpp
    src/waveform_view_model.cpp
    src/qml_registration.cpp)

add_library(usn::app ALIAS usn_app)

target_compile_features(usn_app PUBLIC cxx_std_20)
set_target_properties(usn_app PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    AUTOMOC ON)

target_include_directories(usn_app
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_link_libraries(usn_app
    PUBLIC
        usn::core
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
    PRIVATE
        usn::warnings)
