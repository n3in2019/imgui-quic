#include "imgui_quic.hpp"
#include "imgui_quic.h"
#include "imgui_quic_transport.h"
#include <cstdlib>
#include <arpa/inet.h>

#include <utility>
#include <vector>

namespace imgui_quic {

struct RenderSlot {
    std::function<void()> fn;
};

class Server::Impl {
   public:
    bool init(const Config& config) {
        if (initialized) return true;

        in_addr address{};
        if (!config.port || inet_pton(AF_INET, config.address.c_str(), &address) != 1) return false;
        imgui_quic_config_t cfg = {};
        if (imgui_quic_init(&cfg) != 0) {
            return false;
        }
        backend_initialized = true;
        if (const char* cert = std::getenv("IMGUI_QUIC_CERT")) {
            imgui_quic_transport_config_t transport{};
            transport.host = config.address.c_str();
            transport.port = config.port;
            transport.certificate_file = cert;
            transport.private_key_file = std::getenv("IMGUI_QUIC_KEY");
            transport.token_file = std::getenv("IMGUI_QUIC_TOKEN_FILE");
            transport.allowed_origins = std::getenv("IMGUI_QUIC_ORIGINS");
            if (imgui_quic_start(&transport) != 0) {
                shutdown();
                return false;
            }
        }
        initialized = true;

        return true;
    }

    void shutdown() {
        slots.clear();
        if (backend_initialized) {
            imgui_quic_shutdown();
            backend_initialized = false;
        }
        initialized = false;
    }

    void render() {
        if (!initialized) return;

        imgui_quic_new_frame();

        for (auto it = slots.begin(); it != slots.end();) {
            if (auto slot = it->lock()) {
                slot->fn();
                ++it;
            } else {
                it = slots.erase(it);
            }
        }

        imgui_quic_render();
    }

    std::shared_ptr<RenderSlot> add_slot(std::function<void()> fn) {
        auto slot = std::make_shared<RenderSlot>();
        slot->fn = std::move(fn);
        slots.push_back(slot);
        return slot;
    }

    bool initialized = false;
    bool backend_initialized = false;
    std::vector<std::weak_ptr<RenderSlot>> slots;
};

Server::Server() : impl_(new Impl()) {}

Server::~Server() {
    if (impl_) impl_->shutdown();
}

Server::Server(Server&& other) noexcept = default;

Server& Server::operator=(Server&& other) noexcept = default;

bool Server::init(const Config& config) {
    return impl_->init(config);
}

void Server::shutdown() {
    impl_->shutdown();
}

void Server::render() {
    impl_->render();
}

Server::RenderHandle Server::add_render_callback(std::function<void()> fn) {
    return impl_->add_slot(std::move(fn));
}

}  // namespace imgui_quic
