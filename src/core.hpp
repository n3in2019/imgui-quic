// core.hpp — shared internal declarations for the pure-C++ ImGuiQuic core.
//
// Acknowledged I/P draw frames: 0x0d/0x0e/0x0f, XOR/LZ4 0x22/0x23, planar 0x26/0x27.

#pragma once

#include "imgui_quic_media.h"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace imgui_quic_core {

using ClientId = uint32_t;

struct InputEvent {
    int32_t ev_type = 0;
    float x = 0.0f, y = 0.0f;
    int32_t button = 0;
    int32_t key = 0;
    uint32_t character = 0;
    float wheel_x = 0.0f, wheel_y = 0.0f;
    float display_w = 0.0f, display_h = 0.0f;
};

struct DrawSnapshot;

struct ClientMsg {
    enum class Kind { HelloAck, Input, Resize, ClipboardText, Media, DrawAck };
    Kind kind = Kind::Input;
    uint32_t capabilities = 0;              // HelloAck
    uint32_t draw_ack = 0;                  // 0 requests an I frame
    InputEvent input;                       // Input
    float resize_w = 0.0f;                  // Resize
    float resize_h = 0.0f;                  // Resize
    float resize_scale = 0.0f;              // Resize (devicePixelRatio; 0 = absent)
    imgui_quic_media_info media{};
    std::string clipboard_text;             // ClipboardText
};

// --- Client message parsing (state.cpp) ------------------------------------

std::optional<std::vector<std::pair<ClientId, ClientMsg>>> parse_client_msgs(
    const uint8_t* data, size_t len);
std::optional<uint32_t> leading_hello_ack(
    const std::vector<std::pair<ClientId, ClientMsg>>& msgs, ClientId expected);
std::vector<uint8_t> make_clipboard_write_msg(const std::string& text);

struct FrameHeader {
    float dpx = 0.0f, dpy = 0.0f, dsw = 0.0f, dsh = 0.0f, fbsx = 1.0f, fbsy = 1.0f;
};

std::vector<uint8_t> make_texture_msg(uint64_t id, uint32_t width, uint32_t height,
                                      const uint8_t* pixels, size_t len);

// --- Per-client outbound mailbox --------------------------------------------
//
// Control messages (textures, clipboard) are prerequisites
// of frames, so the sender thread always drains them first. The frame slot
// keeps only the newest encoded frame for that client. P frames reference
// acknowledged snapshots, so replacing an unsent frame is safe.

struct OutBox {
    std::mutex mtx;
    std::deque<std::vector<uint8_t>> control;
    std::shared_ptr<const std::vector<uint8_t>> frame;
    uint64_t frame_seq = 0;
    uint64_t sent_seq = 0;
    std::atomic<bool> closed{false};
    // Installed before publication; immutable for the lifetime of the mailbox.
    std::function<void()> transport_notify;
    void notify() {
        if (transport_notify) transport_notify();
    }
};

struct ClientState {
    std::shared_ptr<const DrawSnapshot> draw_base;
    std::deque<std::shared_ptr<const DrawSnapshot>> draw_pending;
    std::deque<size_t> draw_pending_sizes;
    size_t draw_pending_bytes = 0;
    std::chrono::steady_clock::time_point draw_ack_time{};
    float display_size[2] = {1280.0f, 720.0f};
    float display_scale = 1.0f;  // client devicePixelRatio (layout space stays CSS pixels)
    size_t force_frames = 3;
    // Heartbeat: identical-frame suppression would otherwise let a silently
    // dead connection idle forever (the browser never learns; its socket is
    // still open). Forcing a frame every few seconds makes the sender hit the
    // write error, close the out-box, and trigger the client's reconnect.
    std::chrono::steady_clock::time_point last_send{};
    imgui_quic_media_info media{};
    std::string clipboard_text;
    uint32_t capabilities = 0;
    uint32_t peer_addr = 0;  // network byte order; 0 = unknown
    // Pending input with global arrival sequence numbers; the poller takes
    // the lowest sequence across clients, preserving cross-client FIFO.
    std::deque<std::pair<uint64_t, InputEvent>> input;
    std::shared_ptr<OutBox> out = std::make_shared<OutBox>();
};

// --- Connection limits + capacity caps -----------------------------------------

struct ClientLimits {
    uint32_t max_clients = 0;
    uint32_t max_clients_per_ip = 0;
};

// --- Shared server state (state.cpp) ----------------------------------------

class State {
   public:
    ClientId add_client(uint32_t peer_addr = 0, std::function<void()> notify = {});
    void remove_client(ClientId id);
    bool has_clients() const;

    void set_client_limits(const ClientLimits& limits);
    // Within max_clients / max_clients_per_ip. Friction-level: concurrent
    // handshakes can briefly exceed the cap.
    bool connection_allowed(uint32_t peer_addr) const;

    void push_input(ClientId id, const InputEvent& ev);
    std::optional<InputEvent> try_poll_input();
    void prepare_input_frame(float size[2], float& scale, bool keep_layout = false);

    void set_client_capabilities(ClientId id, uint32_t capabilities);
    void set_display_size(ClientId id, float w, float h, float scale = 0.0f);
    // scale <= 0 means "not provided" and keeps the client's last known value.
    void get_display_size(float out[2]) const;
    float get_display_scale() const;  // active client's devicePixelRatio, 1.0 when unknown

    void on_media(ClientId id, const imgui_quic_media_info& info);
    imgui_quic_media_info get_media() const;
    void control_media(int action, float value);
    void set_clipboard_text(const std::string& text);
    std::string get_clipboard_text() const;
    void on_clipboard_text(ClientId id, const std::string& text);

    void send_control(const std::vector<uint8_t>& data);
    void send_control_to(ClientId id, const std::vector<uint8_t>& data);
    void send_texture(uint64_t id, const std::vector<uint8_t>& msg);
    std::vector<std::vector<uint8_t>> snapshot_textures() const;

    // Render-path entry points.
    bool begin_frame(float dpx, float dpy, float dsw, float dsh, float fbsx, float fbsy);
    bool wants_draw_frames() const;
    void publish_draw_frame(std::vector<uint8_t> bytes);
    void acknowledge_draw(ClientId id, uint32_t frame_id);

    // Test seam: direct access to the client table under its lock.
    template <typename F>
    void with_clients(F&& f) {
        std::lock_guard<std::mutex> lk(clients_mtx_);
        f(clients_);
    }

   private:
    mutable std::mutex clients_mtx_;
    std::unordered_map<ClientId, ClientState> clients_;
    ClientId next_client_id_ = 1;
    // Protected by clients_mtx_. Track consumed presses, including events
    // already handed to ImGui but still waiting in its trickle queue.
    std::unordered_map<int32_t, ClientId> held_buttons_;
    std::unordered_map<int32_t, ClientId> held_keys_;
    std::deque<InputEvent> disconnect_releases_;

    mutable std::mutex active_mtx_;
    std::optional<ClientId> active_client_;

    mutable std::mutex limits_mtx_;
    ClientLimits limits_;

    std::atomic<uint64_t> next_input_seq_{1};

    mutable std::mutex textures_mtx_;
    std::unordered_map<uint64_t, std::vector<uint8_t>> textures_;

    // Canvas size the most recent frame was laid out at (draw-data DisplaySize).
    // Frame-thread only: written by begin_frame, read by try_poll_input.
    float layout_size_[2] = {1280.0f, 720.0f};
    bool input_pointer_valid_ = false;
    float input_pointer_x_ = 0, input_pointer_y_ = 0;
    bool input_pointer_moved_this_frame_ = false;
    bool input_frame_prepared_ = false;
    float input_frame_size_[2] = {1280.0f, 720.0f};
    float input_frame_scale_ = 1.0f;
    uint64_t input_frame_limit_ = 0;
    uint32_t draw_frame_id_ = 0;
};

} // namespace imgui_quic_core
