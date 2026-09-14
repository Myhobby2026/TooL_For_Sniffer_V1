# Codec fuzzing. The iteration count is deliberately overridable: the default keeps
# a developer's `ctest` fast, and the nightly job raises it through the environment
# variable towards the total the master spec asks for.
usn_add_test(fuzz_codec
    SOURCES codec_fuzz_test.cpp
    LIBS usn::transport usn::wire usn::model usn_test_support
    LABELS "fuzz;wire")

set_tests_properties(fuzz_codec PROPERTIES TIMEOUT 3600)
