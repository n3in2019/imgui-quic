add_executable(example_core_cpp_draw example_draw.cpp)
target_link_libraries(example_core_cpp_draw PRIVATE imgui_quic_core_cpp)

add_executable(example_minimal minimal.cpp)
target_link_libraries(example_minimal PRIVATE imgui_quic::core)

add_executable(imgui_quic_dev ../tools/dev/main.cpp)
target_compile_features(imgui_quic_dev PRIVATE cxx_std_17)

find_package(OpenSSL REQUIRED COMPONENTS Crypto)
target_link_libraries(imgui_quic_dev PRIVATE OpenSSL::Crypto)
