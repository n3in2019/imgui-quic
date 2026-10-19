option(IMGUI_QUIC_WEBRTC "Build the optional plain-HTTP WebRTC backend" OFF)
if(IMGUI_QUIC_WEBRTC)
    find_package(LibDataChannel 0.24.1 CONFIG REQUIRED)
    find_package(OpenSSL REQUIRED COMPONENTS Crypto)
    target_compile_definitions(imgui_quic_core_cpp PRIVATE IMGUI_QUIC_WEBRTC=1)
    target_link_libraries(imgui_quic_core_cpp PRIVATE LibDataChannel::LibDataChannel OpenSSL::Crypto)
    # JSON is used only for bounded SDP signaling, never in the draw hot path.
    find_path(IMGUI_QUIC_JSON_INCLUDE nlohmann/json.hpp
        HINTS "${CORE_DIR}/third_party/libdatachannel/deps/json/include" REQUIRED)
    target_include_directories(imgui_quic_core_cpp PRIVATE "${IMGUI_QUIC_JSON_INCLUDE}")
endif()
