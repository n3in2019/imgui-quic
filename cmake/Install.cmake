# --- Install ---------------------------------------------------------------------
# Installs the library, public headers, and a CMake package so consumers can
# find_package(imgui_quic CONFIG REQUIRED) and link imgui_quic::core. The
# installed package embeds Dear ImGui at the pinned revision: imgui.h and
# siblings ship next to the imgui_quic headers, minus upstream imconfig.h
# (the repo's include/imconfig.h is the effective user config via the
# IMGUI_USER_CONFIG define).

file(GLOB IMGUI_QUIC_IMGUI_PUBLIC_HEADERS "${IMGUI_DIR}/*.h")
list(REMOVE_ITEM IMGUI_QUIC_IMGUI_PUBLIC_HEADERS "${IMGUI_DIR}/imconfig.h")

install(TARGETS imgui_quic_core_cpp
    EXPORT imgui_quic-targets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES ${IMGUI_QUIC_IMGUI_PUBLIC_HEADERS}
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES LICENSE
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/imgui_quic")

set(IMGUI_QUIC_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_DATAROOTDIR}/imgui_quic")

install(EXPORT imgui_quic-targets
    NAMESPACE imgui_quic::
    DESTINATION "${IMGUI_QUIC_CMAKE_INSTALL_DIR}")

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/imgui_quicConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/imgui_quicConfig.cmake"
    INSTALL_DESTINATION "${IMGUI_QUIC_CMAKE_INSTALL_DIR}")
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/imgui_quicConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/imgui_quicConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/imgui_quicConfigVersion.cmake"
    DESTINATION "${IMGUI_QUIC_CMAKE_INSTALL_DIR}")

install(DIRECTORY frontend/ DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/imgui_quic/frontend")
