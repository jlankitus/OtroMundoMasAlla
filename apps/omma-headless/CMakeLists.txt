# The headless driver: runs a scenario, emits telemetry, exits with a status
# code. No window, no input, no frame rate. This is the binary CI runs and the
# binary a Monte Carlo sweep launches ten thousand copies of.

add_executable(omma-headless main.cpp)

target_link_libraries(omma-headless
    PRIVATE
        omma::core
        omma::physics
        omma::compile_options
)
