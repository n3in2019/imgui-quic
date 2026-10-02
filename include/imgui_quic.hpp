#pragma once

#include <stdint.h>

#ifdef __cplusplus

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "imgui.h"

namespace imgui_quic {

struct Config {
    std::string address = "127.0.0.1";
    uint16_t port = 4433;
};

class Server {
   public:
    using RenderHandle = std::shared_ptr<void>;

    Server();
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&& other) noexcept;
    Server& operator=(Server&& other) noexcept;

    // Uses IMGUI_QUIC_CERT/KEY/TOKEN_FILE/ORIGINS for transport credentials.
    // Without IMGUI_QUIC_CERT, initializes the rendering core only.
    bool init(const Config& config = {});
    void shutdown();
    void render();

    template <typename DrawFn>
    RenderHandle on_render(DrawFn&& draw) {
        auto fn = std::function<void()>([f = std::forward<DrawFn>(draw)]() mutable { f(); });
        return add_render_callback(std::move(fn));
    }

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;

    RenderHandle add_render_callback(std::function<void()> fn);
};

}  // namespace imgui_quic

#endif
