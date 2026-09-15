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

usn_add_test(wire_format_test
    SOURCES wire_format_test.cpp
    LIBS usn::wire usn_test_support
    LABELS "unit;wire")

usn_add_test(unit_common
    SOURCES common_test.cpp
    LIBS usn::common usn::model usn_test_support
    LABELS "unit;common")

usn_add_test(unit_model
    SOURCES model_test.cpp
    LIBS usn::model usn_test_support
    LABELS "unit;model")

usn_add_test(unit_transport
    SOURCES transport_test.cpp
    LIBS usn::transport usn::common usn::model usn_test_support
    LABELS "unit;transport")

usn_add_test(unit_hal
    SOURCES hal_test.cpp
    LIBS usn::hal usn::core usn_test_support
    LABELS "unit;hal")

usn_add_test(unit_protocol
    SOURCES protocol_test.cpp
    LIBS usn::protocol usn::model usn_test_support
    LABELS "unit;protocol")

usn_add_test(unit_trigger
    SOURCES trigger_test.cpp
    LIBS usn::trigger usn::model usn_test_support
    LABELS "unit;trigger")
