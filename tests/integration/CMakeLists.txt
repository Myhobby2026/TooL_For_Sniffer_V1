# Integration tests: several layers wired together the way the application wires
# them. The device is fake and deterministic; everything above it is real.
usn_add_test(integration_capture
    SOURCES end_to_end_test.cpp
    LIBS usn::core usn::hal usn::transport usn_test_support
    LABELS "integration;concurrency")

# The tool is exercised as a separate process, exactly as a user would run it, so
# the test needs its built path rather than a guess about the working directory.
if(TARGET usn_wire_dumper)
    usn_add_test(integration_wire_dumper
        SOURCES wire_dumper_test.cpp
        LIBS usn::transport usn::wire usn::model usn_test_support
        LABELS "integration;tools")
    target_compile_definitions(integration_wire_dumper PRIVATE
        USN_WIRE_DUMPER_PATH="$<TARGET_FILE:usn_wire_dumper>")
    add_dependencies(integration_wire_dumper usn_wire_dumper)
endif()
