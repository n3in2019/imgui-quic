# LZ4's portable implementation is C++ compatible and built into the core.
set(IMGUI_QUIC_LZ4_DIR "${CORE_DIR}/third_party/lz4" CACHE PATH "Pinned LZ4 source directory")
set(IMGUI_QUIC_LZ4_REVISION "ebb370ca83af193212df4dcbadcc5d87bc0de2f0") # v1.10.0
if(NOT EXISTS "${IMGUI_QUIC_LZ4_DIR}/lib/lz4.c")
    find_package(Git REQUIRED)
    file(MAKE_DIRECTORY "${IMGUI_QUIC_LZ4_DIR}")
    execute_process(COMMAND "${GIT_EXECUTABLE}" init "${IMGUI_QUIC_LZ4_DIR}" COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${IMGUI_QUIC_LZ4_DIR}" fetch --depth 1
        https://github.com/lz4/lz4.git "${IMGUI_QUIC_LZ4_REVISION}" COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${IMGUI_QUIC_LZ4_DIR}" checkout FETCH_HEAD COMMAND_ERROR_IS_FATAL ANY)
endif()
set_source_files_properties("${IMGUI_QUIC_LZ4_DIR}/lib/lz4.c" PROPERTIES LANGUAGE CXX)
