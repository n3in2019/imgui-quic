cmake_minimum_required(VERSION 3.21)
project(imgui_quic VERSION 0.2.0 LANGUAGES CXX)

# Pure-C++ core (I/P draw transport) for ImGuiQuic.

set(CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
include(cmake/Dependencies.cmake)
include(cmake/Lz4.cmake)

find_package(Threads REQUIRED)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
# --- Core library ---------------------------------------------------------------

add_library(imgui_quic_core_cpp
    "${IMGUI_QUIC_LZ4_DIR}/lib/lz4.c"
    src/draw_protocol.cpp
    src/state.cpp
    src/quic.cpp
    src/lib.cpp
    # C++ convenience wrapper (Server + on_render).
    src/imgui_quic.cpp
    src/imgui_quic_imgui.cpp
    # Owns the Dear ImGui context; talks to this library through
    # imgui_quic_core_api_t.
    src/imgui_backend.cpp
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/imgui_demo.cpp")

add_library(imgui_quic::core ALIAS imgui_quic_core_cpp)
set_target_properties(imgui_quic_core_cpp PROPERTIES EXPORT_NAME core)
target_compile_features(imgui_quic_core_cpp PUBLIC cxx_std_17)

option(IMGUI_QUIC_FORCE_SCALAR_QUANTIZATION "Use portable scalar draw quantization" OFF)
if(IMGUI_QUIC_FORCE_SCALAR_QUANTIZATION)
    target_compile_definitions(imgui_quic_core_cpp PRIVATE IMGUI_QUIC_FORCE_SCALAR_QUANTIZATION=1)
endif()

target_include_directories(imgui_quic_core_cpp
    PRIVATE "${CORE_DIR}/src" "${IMGUI_QUIC_LZ4_DIR}/lib"
    PUBLIC
        $<BUILD_INTERFACE:${CORE_DIR}/include>
        $<BUILD_INTERFACE:${IMGUI_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# The wrapper's imgui_quic.hpp uses IMGUI_USER_CONFIG="imconfig.h".
target_compile_definitions(imgui_quic_core_cpp PUBLIC IMGUI_USER_CONFIG="imconfig.h")

target_link_libraries(imgui_quic_core_cpp PUBLIC Threads::Threads)

include(cmake/NativeQuic.cmake)

option(IMGUI_QUIC_BUILD_EXAMPLES "Build the C++ core examples" OFF)
option(IMGUI_QUIC_BUILD_TESTS "Build the C++ core tests" OFF)

if(IMGUI_QUIC_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(IMGUI_QUIC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

include(cmake/Install.cmake)

option(IMGUI_QUIC_BUILD_BENCHMARKS "Build timestamped transport benchmark" OFF)
if(IMGUI_QUIC_BUILD_BENCHMARKS)
    add_executable(transport_benchmark_host tools/benchmarks/transport_host.cpp)
    target_link_libraries(transport_benchmark_host PRIVATE imgui_quic_core_cpp)
endif()
