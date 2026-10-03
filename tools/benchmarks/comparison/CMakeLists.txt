cmake_minimum_required(VERSION 3.21)
project(codec_comparison LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(UPSTREAM "${ROOT}/build/comparison/upstream" CACHE PATH "Pinned upstream checkouts")
set(IMGUI "${ROOT}/third_party/imgui")
set(CORE_DIR "${ROOT}")
include("${ROOT}/cmake/Lz4.cmake")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/compat/imgui")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/compat/imgui/imgui.h" "#include <imgui.h>\n")
# TextureId became GetTexID() in Dear ImGui 1.92. No codec changes.
file(READ "${UPSTREAM}/imgui-ws/src/compressor-xor-rle-per-draw-list-with-vtx-offset.cpp" WS)
string(REPLACE "pcmd->TextureId" "pcmd->GetTexID()" WS "${WS}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ws.cpp" "${WS}")
add_executable(codec codec.cpp "${CMAKE_CURRENT_BINARY_DIR}/ws.cpp"
    "${UPSTREAM}/netImgui/Code/Client/Private/NetImgui_CmdPackets_DrawFrame.cpp"
    "${ROOT}/src/draw_protocol.cpp"
    "${IMGUI}/imgui.cpp" "${IMGUI}/imgui_draw.cpp" "${IMGUI}/imgui_widgets.cpp" "${IMGUI}/imgui_tables.cpp")
target_include_directories(codec PRIVATE "${IMGUI}" "${ROOT}/src" "${ROOT}/include"
    "${UPSTREAM}/imgui-ws/include" "${CMAKE_CURRENT_BINARY_DIR}/compat" "${UPSTREAM}/netImgui/Code/Client")

target_sources(codec PRIVATE "${IMGUI_QUIC_LZ4_DIR}/lib/lz4.c"
    "${CMAKE_CURRENT_BINARY_DIR}/remote_lz4/lz4.c")
target_include_directories(codec PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${IMGUI_QUIC_LZ4_DIR}/lib")
option(IMGUI_QUIC_FORCE_SCALAR_QUANTIZATION "Use portable scalar draw quantization" OFF)
if(IMGUI_QUIC_FORCE_SCALAR_QUANTIZATION)
    target_compile_definitions(codec PRIVATE IMGUI_QUIC_FORCE_SCALAR_QUANTIZATION=1)
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/prepare_remote.py"
    "${UPSTREAM}/remoteimgui/src/app/imgui_remote.h" "${CMAKE_CURRENT_BINARY_DIR}/remote_codec.hpp"
    COMMAND_ERROR_IS_FATAL ANY)

option(BUILD_UPSTREAM_WS_HOST "Build the actual imgui-ws/incppect network stack" OFF)
if(BUILD_UPSTREAM_WS_HOST)
    find_package(Threads REQUIRED)
    set(INCPPECT_NO_SSL ON CACHE BOOL "" FORCE)
    set(INCPPECT_DEBUG OFF CACHE BOOL "" FORCE)
    add_subdirectory("${UPSTREAM}/imgui-ws/third-party/incppect" "incppect")
    add_executable(ws_host ws_host.cpp "${UPSTREAM}/imgui-ws/src/imgui-ws.cpp"
        "${CMAKE_CURRENT_BINARY_DIR}/ws.cpp" "${IMGUI}/imgui.cpp"
        "${IMGUI}/imgui_draw.cpp" "${IMGUI}/imgui_widgets.cpp" "${IMGUI}/imgui_tables.cpp")
    target_include_directories(ws_host PRIVATE "${IMGUI}" "${UPSTREAM}/imgui-ws/include"
        "${CMAKE_CURRENT_BINARY_DIR}/compat")
    target_compile_options(ws_host PRIVATE -include cstdint)
    target_link_libraries(ws_host PRIVATE incppect Threads::Threads)
endif()
