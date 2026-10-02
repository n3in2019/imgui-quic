#include "imgui_quic.hpp"
#include <cassert>
#include <cstdlib>

int main() {
    unsetenv("IMGUI_QUIC_CERT");
    imgui_quic::Server app;
    imgui_quic::Config config;
    config.address = "invalid";
    assert(!app.init(config));
    config.address = "127.0.0.1";
    config.port = 0;
    assert(!app.init(config));
    config.port = 4433;
    setenv("IMGUI_QUIC_CERT", "/nonexistent/imgui-quic-test.pem", 1);
    assert(!app.init(config));
    unsetenv("IMGUI_QUIC_CERT");
    // Failed transport initialization must release the core for a retry.
    assert(app.init());
    app.render();
    app.shutdown();
    config.address = "0.0.0.0";
    config.port = 14433;
    assert(app.init(config));
    app.shutdown();
}
