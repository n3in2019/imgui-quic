# Overridable so package managers can pre-seed third_party/imgui with the
# pinned revision; the fetch below then stays a no-op.
set(IMGUI_DIR "${CORE_DIR}/third_party/imgui" CACHE PATH
    "Dear ImGui source directory (the pinned revision is fetched here when missing)")
set(IMGUI_REVISION "b61e56346a92cfcaf1f43a545ca37b0b32239654")

# Fetch the pinned Dear ImGui docking revision on first configure. The
# directory is local dependency state and must not be committed.
if(NOT EXISTS "${IMGUI_DIR}/imgui.h")
    find_package(Git REQUIRED)
    message(STATUS "Fetching pinned Dear ImGui (${IMGUI_REVISION})...")
    file(MAKE_DIRECTORY "${CORE_DIR}/third_party")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" init "${IMGUI_DIR}"
        RESULT_VARIABLE imgui_init_result)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${IMGUI_DIR}" fetch --depth 1
                https://github.com/ocornut/imgui.git "${IMGUI_REVISION}"
        RESULT_VARIABLE imgui_fetch_result)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${IMGUI_DIR}" checkout FETCH_HEAD
        RESULT_VARIABLE imgui_checkout_result)
    if(NOT imgui_init_result EQUAL 0 OR NOT imgui_fetch_result EQUAL 0
            OR NOT imgui_checkout_result EQUAL 0)
        message(FATAL_ERROR
            "Failed to fetch Dear ImGui ${IMGUI_REVISION} into ${IMGUI_DIR}. "
            "Check network access and Git availability, then reconfigure.")
    endif()
endif()
