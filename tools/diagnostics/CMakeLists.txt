add_executable(usn_diagnostics
    main.cpp)

target_compile_features(usn_diagnostics PRIVATE cxx_std_20)
set_target_properties(usn_diagnostics PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    OUTPUT_NAME diagnostics)

target_link_libraries(usn_diagnostics PRIVATE
    usn::core
    usn::hal
    usn::transport
    usn::model
    usn::common
    usn::warnings
    nlohmann_json::nlohmann_json
    fmt::fmt)
