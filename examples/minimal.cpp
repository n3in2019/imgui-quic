// Start here: ordinary native Dear ImGui, displayed in a browser.
#include "support.hpp"
#include <chrono>
#include <thread>

int main(int argc, char** argv) {
    imgui_quic::Server app;
    if (!example::start(app, argc, argv)) return 1;

    int clicks = 0;
    auto ui = app.on_render([&] {
        ImGui::Begin("Hello, ImGuiQuic");
        ImGui::TextUnformatted("Native C++ / WebTransport / WebGL");
        if (ImGui::Button("Click me")) ++clicks;
        ImGui::Text("Clicks: %d", clicks);
        ImGui::End();
    });

    while (!example::stop_requested) {
        app.render();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}
