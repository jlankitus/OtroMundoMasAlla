# The terminal front-end: one client of the simulator, not a layer of it.
# omma_physics does not know this target exists, and a Unity or Unreal
# front-end would sit right here beside it.

add_executable(omma-ascii
    main.cpp
    draw.cpp
    options.cpp
    scenarios.cpp
    bodies.hpp
    draw.hpp
    options.hpp
    scenarios.hpp
    view.hpp
)

target_include_directories(omma-ascii PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(omma-ascii
    PRIVATE
        omma::core
        omma::physics
        omma::render
        omma::sim
        omma::compile_options
)
