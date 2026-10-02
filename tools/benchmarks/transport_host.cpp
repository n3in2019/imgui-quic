// Timestamped draw workload for the isolated native WebTransport benchmark.
#include "imgui_quic.hpp"
#include "imgui_quic_transport.h"
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <thread>
using Clock = std::chrono::steady_clock;
volatile std::sig_atomic_t running = 1;
void stop(int) { running = 0; }
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    imgui_quic::Config config;
    const bool dynamic = std::atoi(argv[1]);
    const int fps = std::atoi(argv[2]);
    imgui_quic::Server app;
    if (!app.init(config) || fps < 1) return 1;
    if (const char* cert = std::getenv("IMGUI_QUIC_CERT")) {
        imgui_quic_transport_config_t quic{};
        quic.certificate_file = cert;
        quic.private_key_file = std::getenv("IMGUI_QUIC_KEY");
        quic.token_file = std::getenv("IMGUI_QUIC_TOKEN_FILE");
        quic.allowed_origins = "http://localhost";
        quic.port = 19443;
        if (imgui_quic_start(&quic) != 0) return 1;
    }
    std::signal(SIGTERM,stop);
    unsigned tick = 0;
    auto callback = app.on_render([&] {
        // Three vertex colors carry exact generation time and consumed input ID.
        // This costs at most 12 changed bytes per frame in the measured stream.
        auto* marker = ImGui::GetBackgroundDrawList();
        int start = marker->VtxBuffer.Size;
        marker->AddTriangleFilled(ImVec2(1,1),ImVec2(3,1),ImVec2(1,3),0xffffffff);
        const auto stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
        marker->VtxBuffer[start].col = uint32_t(stamp);
        marker->VtxBuffer[start+1].col = uint32_t(uint64_t(stamp)>>32);
        marker->VtxBuffer[start+2].col = uint32_t(std::fmax(0.0f,ImGui::GetIO().MousePos.x));
        ImGui::SetNextWindowPos(ImVec2(dynamic ? 30 : 130+int(100*std::sin(tick*.04)),30));
        ImGui::SetNextWindowSize(ImVec2(1000,650));
        ImGui::Begin("Transport benchmark",nullptr,ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoResize);
        ImGui::TextUnformatted("Native draw data / identical codec / 30 FPS");
        for (unsigned row=0; row<30; ++row) {
            ImGui::Text("Row %02u: value %08u",row,dynamic ? tick*1234567u+row : row);
        }
        ImGui::End(); ++tick;
    });
    auto next = Clock::now();
    while (running) {
        app.render();
        next += std::chrono::nanoseconds(1000000000/fps);
        std::this_thread::sleep_until(next);
    }
}
