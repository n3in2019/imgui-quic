// Timestamped draw workload for the isolated native WebTransport benchmark.
#include "imgui_quic.hpp"
#include "imgui_quic_transport.h"
#include "comparison/scenes.hpp"
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
    config.port = 19443;
    const std::string certificate=std::getenv("IMGUI_QUIC_CERT")?std::getenv("IMGUI_QUIC_CERT"):"";
    // Fanout exercises independent clients from one loopback address. Override
    // only this benchmark's per-address admission limit, not the library default.
    const bool fanout=std::getenv("IMGW_BENCH_FANOUT")!=nullptr;
    if(!certificate.empty() && fanout) unsetenv("IMGUI_QUIC_CERT");
    if (!certificate.empty()) setenv("IMGUI_QUIC_ORIGINS", "http://localhost", 1);
    const std::string scene=std::string(argv[1])=="0"?"moving":std::string(argv[1])=="1"?"dynamic":argv[1];
    const int fps = std::atoi(argv[2]);
    imgui_quic::Server app;
    if (!app.init(config) || fps < 1) return 1;
    if(!certificate.empty() && fanout) {
        imgui_quic_transport_config_t transport{};
        transport.host="127.0.0.1";transport.port=config.port;
        transport.certificate_file=certificate.c_str();
        transport.private_key_file=std::getenv("IMGUI_QUIC_KEY");
        transport.token_file=std::getenv("IMGUI_QUIC_TOKEN_FILE");
        transport.allowed_origins="http://localhost";
        transport.max_clients_per_ip=8;
        if(imgui_quic_start(&transport)!=0)return 1;
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
        benchmark_scene::draw(scene,tick++);
    });
    auto next = Clock::now();
    while (running) {
        app.render();
        next += std::chrono::nanoseconds(1000000000/fps);
        std::this_thread::sleep_until(next);
    }
}
