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
    if (!std::getenv("IMGUI_QUIC_CERT")) {
        std::fprintf(stderr, "Missing QUIC credentials; use build/examples/imgui_quic_dev\n");
        return false;
    }
    imgui_quic::Config config;
    if (const char* host = std::getenv("IMGUI_QUIC_HOST")) config.address = host;
    if (const char* port = std::getenv("IMGUI_QUIC_PORT")) {
        if (!parse_port(port, config.port)) return false;
    }
    if (!app.init(config)) {
        std::fprintf(stderr, "Failed to initialize ImGuiQuic\n");
        return false;
    }

    return true;
}
} // namespace example
