# Unit tests: one binary per layer so a failure names the layer that broke.

usn_add_test(unit_timeline
    SOURCES timeline_test.cpp
    LIBS usn::core usn_test_support
    LABELS "unit;core")

usn_add_test(unit_diagnostics_log
    SOURCES diagnostics_log_test.cpp
    LIBS usn::core usn_test_support
    LABELS "unit;core")

usn_add_test(unit_config
    SOURCES config_test.cpp
    LIBS usn::core usn_test_support
    LABELS "unit;core")

usn_add_test(unit_storage
    SOURCES istorage_test.cpp
    LIBS usn::core usn_test_support
    LABELS "unit;core")


usn_add_test(unit_capture_pipeline
    SOURCES capture_pipeline_test.cpp
    LIBS usn::core usn_test_support
    LABELS "unit;core;concurrency")

usn_add_test(unit_capture_session
    SOURCES capture_session_test.cpp
    LIBS usn::core usn::hal usn_test_support
    LABELS "unit;core;concurrency")

usn_add_test(unit_wire_codec
    SOURCES wire_codec_test.cpp
    LIBS usn::transport usn::wire usn::model usn_test_support
    LABELS "unit;wire")
