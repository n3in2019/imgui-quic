# The ImGui core stays static; the QUICHE dependency is a private shared object
# with only a C ABI exported, so it cannot duplicate ImGui/dependency symbols.
set(VCPKG_POLICY_DLLS_IN_STATIC_LIBRARY enabled)

if(VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
    # The library embeds its own Dear ImGui copy at a pinned revision; a
    # shared build would duplicate ImGui symbols across DSOs. Static only.
    message(STATUS "imgui-quic: forcing static library linkage")
    set(VCPKG_LIBRARY_LINKAGE static)
endif()

# Development overlay builds the explicitly selected checkout.
if(DEFINED ENV{IMGUI_QUIC_LOCAL_SOURCE_DIR})
    set(IMGUI_QUIC_LOCAL "$ENV{IMGUI_QUIC_LOCAL_SOURCE_DIR}")
    set(SOURCE_PATH "${CURRENT_BUILDTREES_DIR}/src/local")
    file(REMOVE_RECURSE "${SOURCE_PATH}")
    file(COPY
        "${IMGUI_QUIC_LOCAL}/CMakeLists.txt"
        "${IMGUI_QUIC_LOCAL}/LICENSE"
        "${IMGUI_QUIC_LOCAL}/cmake"
        "${IMGUI_QUIC_LOCAL}/include"
        "${IMGUI_QUIC_LOCAL}/src"
        "${IMGUI_QUIC_LOCAL}/frontend"
        "${IMGUI_QUIC_LOCAL}/transport"
        DESTINATION "${SOURCE_PATH}")
else()
    message(FATAL_ERROR "Set IMGUI_QUIC_LOCAL_SOURCE_DIR to the checkout for this development overlay port")
endif()

# Dear ImGui is vendored at the exact commit pinned by upstream's CMake
# (v1.92.8, docking branch): generated native C bindings require this
# revision, so the port cannot track the standalone imgui port. Pre-seeding
# keeps the configure-time fetch a no-op.
vcpkg_from_git(
    OUT_SOURCE_PATH IMGUI_SOURCE_PATH
    URL "https://github.com/ocornut/imgui.git"
    REF "b61e56346a92cfcaf1f43a545ca37b0b32239654"
)
file(COPY "${IMGUI_SOURCE_PATH}/" DESTINATION "${SOURCE_PATH}/third_party/imgui")

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DIMGUI_QUIC_BUILD_EXAMPLES=OFF
        -DIMGUI_QUIC_BUILD_TESTS=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME imgui_quic)

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(
    COMMENT "ImGuiQuic is MIT licensed. It embeds Dear ImGui (MIT) and generated dear_bindings outputs (MIT)."
    FILE_LIST
        "${SOURCE_PATH}/LICENSE"
        "${IMGUI_SOURCE_PATH}/LICENSE.txt"
)
