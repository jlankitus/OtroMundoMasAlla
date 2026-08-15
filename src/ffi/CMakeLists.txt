# omma::ffi — the C ABI shared library game engines load. Unity P/Invokes
# omma.dll; Unreal can link it or the C++ targets directly. Pure C surface:
# no exceptions, no C++ types across the boundary.

add_library(omma_ffi SHARED
    omma_c.cpp
    omma_c.h
)
add_library(omma::ffi ALIAS omma_ffi)

set_target_properties(omma_ffi PROPERTIES OUTPUT_NAME omma)
target_compile_definitions(omma_ffi PRIVATE OMMA_FFI_EXPORTS)

target_link_libraries(omma_ffi
    PUBLIC  omma::sim
    PRIVATE omma::compile_options
)
