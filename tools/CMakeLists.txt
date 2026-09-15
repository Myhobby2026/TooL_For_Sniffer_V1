# ----------------------------------------------------------------------------
# tools -- command-line utilities that exercise the same libraries the GUI uses.
#
# These are not demos. wire_dumper is how a real Teensy capture that misbehaves in
# the field gets diagnosed without the GUI in the loop, which is also what makes it
# useful as a cross-check on the codec tests.
# ----------------------------------------------------------------------------
add_subdirectory(wire_dumper)
add_subdirectory(diagnostics)
