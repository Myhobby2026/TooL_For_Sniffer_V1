// -----------------------------------------------------------------------------
// mini_gtest_main.cpp -- the entry point the shim provides in place of
// gtest_main. The banner is printed on every run so that a log from a shim build
// cannot be mistaken for a log from a real GoogleTest build.
// -----------------------------------------------------------------------------
#include <iostream>

#include "gtest/gtest.h"

int main(int argc, char** argv) {
    std::cout
        << "================================================================\n"
        << " TEST FRAMEWORK: mini_gtest -- a local subset shim, NOT GoogleTest.\n"
        << " Configured with -DUSN_TEST_FRAMEWORK=mini because GoogleTest could\n"
        << " not be provisioned in this environment. Results from this run do\n"
        << " NOT satisfy the phase gate on their own; re-run with real\n"
        << " GoogleTest (USN_TEST_FRAMEWORK=gtest, the default) before sign-off.\n"
        << "================================================================"
        << std::endl;
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
