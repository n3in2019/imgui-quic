#pragma once

// Example-only process setup. Applications use the public lifecycle/QUIC APIs.
#include "imgui_quic.hpp"
#include "imgui_quic_transport.h"
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace example {
inline volatile std::sig_atomic_t stop_requested = 0;
inline void request_stop(int) { stop_requested = 1; }

inline bool parse_port(const char* text, uint16_t& port) {
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (!*text || *end || value < 1 || value > 65535) {
        std::fprintf(stderr, "Port must be between 1 and 65535\n");
        return false;
    }
    port = uint16_t(value);
    return true;
}

inline bool start(imgui_quic::Server& app, int argc, char** argv) {
    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);
    if (argc != 1) {
        std::fprintf(stderr, "Configure QUIC using IMGUI_QUIC_* environment variables\n");
        return false;
    }
    imgui_quic::Config config;
    if (!app.init(config)) {
        fprintf(stderr, "Failed to initialize ImGuiQuic (C++ core)\n");
        return false;
    }

    // Start the in-process QUIC endpoint with the configured credentials.
    if(const char* cert=std::getenv("IMGUI_QUIC_CERT")) {
        imgui_quic_transport_config_t quic{};
        quic.host=std::getenv("IMGUI_QUIC_HOST");
        quic.certificate_file=cert;
        quic.private_key_file=std::getenv("IMGUI_QUIC_KEY");
        quic.token_file=std::getenv("IMGUI_QUIC_TOKEN_FILE");
        quic.allowed_origins=std::getenv("IMGUI_QUIC_ORIGINS");
        if(const char* port=std::getenv("IMGUI_QUIC_PORT")) {
            if (!parse_port(port, quic.port)) return false;
        }
        if(imgui_quic_start(&quic)!=0)return false;
    } else {
        std::fprintf(stderr, "Missing QUIC credentials; use build/examples/imgui_quic_dev\n");
        return false;
    }

    return true;
}
} // namespace example
