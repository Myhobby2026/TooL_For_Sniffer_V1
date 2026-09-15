add_executable(universal_sniffer
    src/main.cpp
    src/panel_registry.cpp
    src/layout_description.cpp)

target_compile_features(universal_sniffer PRIVATE cxx_std_20)
set_target_properties(universal_sniffer PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    AUTOMOC ON)

target_include_directories(universal_sniffer
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(universal_sniffer
    PRIVATE
        usn::app
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        nlohmann_json::nlohmann_json
        usn::warnings)

usn_add_qml_module(universal_sniffer
    URI "UniversalSniffer.Gui"
    VERSION "1.0"
    QML_FILES
        qml/Main.qml
        qml/panels/DiagnosticsPanel.qml
        qml/panels/CapturePanel.qml
        qml/theme/Theme.qml
        qml/theme/qmldir)
