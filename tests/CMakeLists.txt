add_executable(core_cpp_tests protocol_tests.cpp)
target_link_libraries(core_cpp_tests PRIVATE imgui_quic_core_cpp)
enable_testing()
add_test(NAME protocol COMMAND core_cpp_tests)

enable_language(C)
add_executable(c_api_tests c_api_tests.c)
target_link_libraries(c_api_tests PRIVATE imgui_quic_core_cpp)
add_test(NAME c_api COMMAND c_api_tests)
set_tests_properties(c_api PROPERTIES RUN_SERIAL TRUE)

# Assertions in test binaries must remain active in Release builds.
foreach(test_target core_cpp_tests c_api_tests)
    target_compile_options(${test_target} PRIVATE -UNDEBUG)
endforeach()

add_executable(draw_protocol_tests draw_protocol_tests.cpp)
target_link_libraries(draw_protocol_tests PRIVATE imgui_quic_core_cpp)
target_compile_options(draw_protocol_tests PRIVATE -UNDEBUG)
add_test(NAME draw_protocol COMMAND draw_protocol_tests)

add_executable(quic_protocol_tests quic_protocol_tests.cpp)
target_compile_options(quic_protocol_tests PRIVATE -UNDEBUG)
add_test(NAME quic_protocol COMMAND quic_protocol_tests)

# Internal protocol fixtures deliberately use private core headers.
foreach(test_target core_cpp_tests draw_protocol_tests)
    target_include_directories(${test_target} PRIVATE ../src)
endforeach()

target_compile_features(quic_protocol_tests PRIVATE cxx_std_17)

if(TARGET imgui_quic_dev)
    add_test(NAME dev_launcher COMMAND "${CMAKE_COMMAND}"
        "-DLAUNCHER=$<TARGET_FILE:imgui_quic_dev>"
        "-DDIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/dev-credentials"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/dev_launcher.cmake")
endif()

add_executable(server_config_tests server_config_tests.cpp)
target_link_libraries(server_config_tests PRIVATE imgui_quic::core)
target_compile_options(server_config_tests PRIVATE -UNDEBUG)
add_test(NAME server_config COMMAND server_config_tests)
