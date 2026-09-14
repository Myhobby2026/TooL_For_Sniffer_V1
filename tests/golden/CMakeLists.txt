# Golden-file tests. USN_GOLDEN_DIR is passed in so the test never has to guess
# where the source tree is relative to the working directory CTest uses.
usn_add_test(golden_wire_packet
    SOURCES wire_golden_test.cpp
    LIBS usn::transport usn::wire usn::model usn_test_support nlohmann_json::nlohmann_json
    LABELS "golden;wire")

target_compile_definitions(golden_wire_packet PRIVATE
    USN_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
