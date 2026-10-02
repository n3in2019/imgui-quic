#include "quic.hpp"

#include <cstdio>

#include "../transport/quiche/bridge.h"
#include "draw_protocol.hpp"
#ifdef IMGUI_QUIC_NATIVE_QUIC
#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#endif
namespace imgui_quic_core {
#ifdef IMGUI_QUIC_NATIVE_QUIC
struct QuicWake {
    int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    ~QuicWake() {
        if (fd >= 0) ::close(fd);
    }
    void notify() const noexcept {
        const uint64_t one = 1;
        ssize_t result;
        do {
            result = ::write(fd, &one, sizeof(one));
        } while (result < 0 && errno == EINTR);
        // EAGAIN means a wake is already pending; no work can be lost.
    }
};
struct QuicHandle {
    std::shared_ptr<State> state;
    std::shared_ptr<QuicWake> wake = std::make_shared<QuicWake>();
    void* runtime = nullptr;
    ~QuicHandle() { imgw_quic_stop(runtime); }
};
namespace {
State& state(uintptr_t ctx) {
    return *reinterpret_cast<QuicHandle*>(ctx)->state;
}
uint32_t connect(uintptr_t ctx, uint32_t peer) noexcept {
    try {
        auto wake = reinterpret_cast<QuicHandle*>(ctx)->wake;
        if (state(ctx).connection_allowed(peer))
            return state(ctx).add_client(peer, [wake] { wake->notify(); });
    } catch (...) {
    }
    return 0;
}
void disconnect(uintptr_t ctx, uint32_t id) noexcept {
    try {
        state(ctx).remove_client(id);
    } catch (...) {
    }
}
bool input(uintptr_t ctx, uint32_t id, const uint8_t* data, size_t len) noexcept {
    try {
        auto& s = state(ctx);
        auto msgs = parse_client_msgs(data, len);
        if (!msgs) return false;
        for (const auto& item : *msgs)
            if (item.first != id) return false;
        bool negotiated = false, exists = false;
        s.with_clients([&](auto& clients) {
            auto it = clients.find(id);
            if (it != clients.end()) {
                exists = true;
                negotiated = it->second.capabilities != 0;
            }
        });
        if (!exists) return false;
        if (!negotiated) {
            auto caps = leading_hello_ack(*msgs, id);
            if (!caps) return true;
            if (!(*caps & kDrawIPCapability)) return false;
            s.set_client_capabilities(id, *caps);
            for (const auto& texture : s.snapshot_textures()) s.send_control_to(id, texture);
            return true;  // discard early trailing input, as on the native protocol
        }
        for (auto& [cid, msg] : *msgs) switch (msg.kind) {
                case ClientMsg::Kind::DrawAck:
                    s.acknowledge_draw(cid, msg.draw_ack);
                    break;
                case ClientMsg::Kind::HelloAck:
                    break;
                case ClientMsg::Kind::Input:
                    s.push_input(cid, msg.input);
                    break;
                case ClientMsg::Kind::Resize:
                    s.set_display_size(cid, msg.resize_w, msg.resize_h, msg.resize_scale);
                    break;
                case ClientMsg::Kind::Media:
                    s.on_media(cid, msg.media);
                    break;
                case ClientMsg::Kind::ClipboardText:
                    s.on_clipboard_text(cid, msg.clipboard_text);
                    break;
            }
        return true;
    } catch (...) {
        return false;
    }
}
int poll(uintptr_t ctx, uint32_t id, const uint8_t** data, size_t* len, void** cookie) noexcept {
    try {
        std::shared_ptr<OutBox> out;
        state(ctx).with_clients([&](auto& clients) {
            auto it = clients.find(id);
            if (it != clients.end()) out = it->second.out;
        });
        if (!out || out->closed) return -1;
        std::lock_guard<std::mutex> lock(out->mtx);
        std::shared_ptr<const std::vector<uint8_t>> packet;
        if (!out->control.empty()) {
            packet = std::make_shared<const std::vector<uint8_t>>(std::move(out->control.front()));
            out->control.pop_front();
        } else if (out->frame_seq != out->sent_seq) {
            packet = std::move(out->frame);
            out->sent_seq = out->frame_seq;
        }
        if (!packet) return 0;
        auto owner = new std::shared_ptr<const std::vector<uint8_t>>(std::move(packet));
        *data = (*owner)->data();
        *len = (*owner)->size();
        *cookie = owner;
        return 1;
    } catch (...) {
        return -1;
    }
}
void release(void* cookie) noexcept {
    delete static_cast<std::shared_ptr<const std::vector<uint8_t>>*>(cookie);
}
}  // namespace
std::shared_ptr<QuicHandle> start_quic(std::shared_ptr<State> state,
                                       const imgui_quic_transport_config_t& c) {
    if (state->has_clients()) {
        fprintf(stderr, "[imgui_quic] QUIC must start before clients\n");
        return nullptr;
    }
    auto server = std::make_shared<QuicHandle>();
    server->state = std::move(state);
    if (server->wake->fd < 0) {
        perror("[imgui_quic] eventfd");
        return nullptr;
    }
    QuicCallbacks callbacks{reinterpret_cast<uintptr_t>(server.get()),
                            server->wake->fd,
                            connect,
                            disconnect,
                            input,
                            poll,
                            release};
    server->runtime = imgw_quic_start(c.host ? c.host : "127.0.0.1", c.port ? c.port : 4433,
                                      c.certificate_file, c.private_key_file, c.token_file,
                                      c.allowed_origins, c.max_clients ? c.max_clients : 8,
                                      c.max_clients_per_ip ? c.max_clients_per_ip : 2, callbacks);
    if (!server->runtime) return nullptr;
    fprintf(stderr, "[imgui_quic] Native WebTransport listening on https://%s:%u/wt\n",
            c.host ? c.host : "127.0.0.1", unsigned(c.port ? c.port : 4433));
    return server;
}
#else
struct QuicHandle {};
std::shared_ptr<QuicHandle> start_quic(std::shared_ptr<State>, const imgui_quic_transport_config_t&) {
    fprintf(stderr, "[imgui_quic] Native QUIC disabled in this build\n");
    return nullptr;
}
#endif
}  // namespace imgui_quic_core
