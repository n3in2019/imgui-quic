# Native WebTransport is compiled into the host process through a small C ABI.
option(IMGUI_QUIC_NATIVE_QUIC "Build native Google QUICHE WebTransport" ON)
if(IMGUI_QUIC_NATIVE_QUIC)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
       NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64|aarch64|arm64)$")
        message(FATAL_ERROR "Native QUICHE supports Linux x86-64/AArch64; use IMGUI_QUIC_NATIVE_QUIC=OFF elsewhere")
    endif()
    if(CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR "Build QUICHE natively on the target architecture (AArch64 is covered by CI); cross compilation is not configured")
    endif()
    set(IMGUI_QUIC_QUICHE_SOURCE "${CORE_DIR}/third_party/google-quiche" CACHE PATH "Pinned Google QUICHE build checkout")
    set(IMGUI_QUIC_QUICHE_JOBS 6 CACHE STRING "Concurrent QUICHE compiler jobs")
    set(QUIC_LIBRARY "${CMAKE_CURRENT_BINARY_DIR}/libimgui_quic_quiche.so")
    set(QUIC_NOTICES "${CMAKE_CURRENT_BINARY_DIR}/QUIC_THIRD_PARTY_NOTICES.txt")
    set(quiche_arch x86_64)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
        set(quiche_arch arm64)
    endif()
    add_custom_command(OUTPUT "${QUIC_LIBRARY}" "${QUIC_NOTICES}"
        COMMAND "${CMAKE_COMMAND}" "-DROOT=${CORE_DIR}"
                "-DBUILD=${CMAKE_CURRENT_BINARY_DIR}" "-DSOURCE=${IMGUI_QUIC_QUICHE_SOURCE}"
                "-DJOBS=${IMGUI_QUIC_QUICHE_JOBS}" "-DARCH=${quiche_arch}"
                -P "${CORE_DIR}/cmake/BuildQuiche.cmake"
        DEPENDS transport/quiche/server.cc transport/quiche/bridge.h transport/quiche/protocol.h
                transport/quiche/exports.map transport/quiche/BUILD.fragment transport/quiche/dependencies.json
                cmake/BuildQuiche.cmake cmake/QuicNotices.cmake
        COMMENT "Building Google QUICHE WebTransport (native architecture)" VERBATIM)
    add_custom_target(imgui_quic_quic_build DEPENDS "${QUIC_LIBRARY}" "${QUIC_NOTICES}")
    add_library(imgui_quic::quic SHARED IMPORTED GLOBAL)
    set_target_properties(imgui_quic::quic PROPERTIES IMPORTED_LOCATION "${QUIC_LIBRARY}")
    add_dependencies(imgui_quic_core_cpp imgui_quic_quic_build)
    target_compile_definitions(imgui_quic_core_cpp PRIVATE IMGUI_QUIC_NATIVE_QUIC=1)
    target_link_libraries(imgui_quic_core_cpp PRIVATE imgui_quic::quic)
    install(FILES "${QUIC_LIBRARY}" DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    install(FILES "${QUIC_NOTICES}" DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/imgui_quic")
endif()
