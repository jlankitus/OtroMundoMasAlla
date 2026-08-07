# The terminal front-end. One of several possible clients of the simulator,
# not a layer of it -- omma_physics does not know this target exists, and a
# Unity or Unreal front-end would sit right here beside it.
#
# All that lives here is the glue: the game loop, key bindings, and the
# decision about which glyph a planet gets. Everything reusable is in
# omma::render.

add_executable(omma-ascii main.cpp)

target_link_libraries(omma-ascii
    PRIVATE
        omma::core
        omma::physics
        omma::render
        omma::compile_options
)
