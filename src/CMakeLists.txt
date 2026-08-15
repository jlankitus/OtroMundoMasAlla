# Each layer is its own target so the dependency direction is enforced by the
# build system, not by discipline:
#
#     core  <---  physics  <---  sim  <---  apps
#
# core knows nothing about orbits. physics knows nothing about scheduling.
# sim knows nothing about rendering. If someone ever writes an #include that
# points the wrong way up this chain, the link step fails and we find out
# immediately instead of six months later.

add_subdirectory(core)
add_subdirectory(physics)
add_subdirectory(render)
add_subdirectory(sim)

add_subdirectory(ffi)
