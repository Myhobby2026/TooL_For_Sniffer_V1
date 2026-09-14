# ----------------------------------------------------------------------------
# desktop -- the host application. Layers are added bottom-up; each one may only
# depend on layers already listed above it (docs/architecture_review.md 1.1).
# ----------------------------------------------------------------------------
add_subdirectory(common)
add_subdirectory(model)
add_subdirectory(transport)
add_subdirectory(trigger)
add_subdirectory(hal)
add_subdirectory(protocol)
add_subdirectory(core)
