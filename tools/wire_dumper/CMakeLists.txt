add_executable(usn_wire_dumper main.cpp)

target_compile_features(usn_wire_dumper PRIVATE cxx_std_20)
set_target_properties(usn_wire_dumper PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    OUTPUT_NAME wire_dumper)

target_link_libraries(usn_wire_dumper
    PRIVATE
        usn::transport
        usn::model
        usn::common
        usn::warnings
        fmt::fmt)

install(TARGETS usn_wire_dumper RUNTIME DESTINATION bin)
