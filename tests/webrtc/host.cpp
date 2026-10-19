#include "../../examples/support.hpp"
#include "imgui_quic_texture.h"
#include <chrono>
#include <thread>
#include <vector>
int main(int argc, char** argv) {
    imgui_quic::Server app;
    if (!example::start(app, argc, argv)) return 1;
    // Larger than the negotiated SCTP message size: exercise reliable chunking.
    std::vector<uint8_t> pixels(256*128*4, 255);
    const auto texture = imgui_quic_texture_upload_rgba(0,256,128,pixels.data(),pixels.size());
    if (!texture) return 2;
    unsigned ticks = 0, clicks = 0;
    auto ui = app.on_render([&] {
        ImGui::SetNextWindowPos({20,20}); ImGui::SetNextWindowSize({320,240});
        ImGui::Begin("HTTP WebRTC acceptance", nullptr, ImGuiWindowFlags_NoSavedSettings);
        ImGui::SetCursorScreenPos({40,80});
        if (ImGui::Button("Click", {100,40})) { std::printf("clicks=%u\n", ++clicks); std::fflush(stdout); }
        ImGui::Image(ImTextureRef((ImTextureID)texture), {64,32});
        ImGui::End();
        auto* draw=ImGui::GetBackgroundDrawList();
        const float x=400+float(ticks++%100);
        draw->AddRectFilled({x,60},{x+30,100},0xff00ffff);
    });
    while (!example::stop_requested) { app.render(); std::this_thread::sleep_for(std::chrono::milliseconds(16)); }
}
