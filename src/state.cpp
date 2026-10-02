// state.cpp — shared server state: clients, input queue, clipboard, textures,
// and the frame path. Frames contain native ImGui draw data.

#include "core.hpp"
#include "draw_protocol.hpp"
#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <iterator>

namespace imgui_quic_core {

namespace {

// Little-endian readers over a (ptr, len) view: client payloads are parsed in
// place, without copying the receive buffer.

bool read_f32(const uint8_t* d, size_t len, size_t off, float& out) {
    if (off + 4 > len) return false;
    uint32_t bits = uint32_t(d[off]) | (uint32_t(d[off + 1]) << 8) | (uint32_t(d[off + 2]) << 16) |
                    (uint32_t(d[off + 3]) << 24);
    memcpy(&out, &bits, 4);
    return true;
}

bool read_u16(const uint8_t* d, size_t len, size_t off, uint16_t& out) {
    if (off + 2 > len) return false;
    out = uint16_t(d[off]) | (uint16_t(d[off + 1]) << 8);
    return true;
}

bool read_u32(const uint8_t* d, size_t len, size_t off, uint32_t& out) {
    if (off + 4 > len) return false;
    out = uint32_t(d[off]) | (uint32_t(d[off + 1]) << 8) | (uint32_t(d[off + 2]) << 16) |
          (uint32_t(d[off + 3]) << 24);
    return true;
}

std::optional<ClientMsg> parse_client_payload(uint8_t type, const uint8_t* d, size_t len) {
    ClientMsg msg;
    switch (type) {
        case 0x10: {  // mouse move
            msg.kind = ClientMsg::Kind::Input;
            msg.input.ev_type = 0;
            if (!read_f32(d, len, 0, msg.input.x) || !read_f32(d, len, 4, msg.input.y))
                return std::nullopt;
            if (!std::isfinite(msg.input.x) || !std::isfinite(msg.input.y)) return std::nullopt;
            return msg;
        }
        case 0x11:  // mouse down
        case 0x12: {  // mouse up
            msg.kind = ClientMsg::Kind::Input;
            msg.input.ev_type = (type == 0x11) ? 1 : 2;
            if (len < 1) return std::nullopt;
            msg.input.button = int32_t(d[0]);
            if (msg.input.button >= ImGuiMouseButton_COUNT) return std::nullopt;
            return msg;
        }
        case 0x13: {  // mouse wheel
            msg.kind = ClientMsg::Kind::Input;
            msg.input.ev_type = 3;
            if (!read_f32(d, len, 0, msg.input.wheel_x) || !read_f32(d, len, 4, msg.input.wheel_y))
                return std::nullopt;
            if (!std::isfinite(msg.input.wheel_x) || !std::isfinite(msg.input.wheel_y)) return std::nullopt;
            return msg;
        }
        case 0x14:  // key down
        case 0x15: {  // key up
            msg.kind = ClientMsg::Kind::Input;
            msg.input.ev_type = (type == 0x14) ? 4 : 5;
            uint16_t key;
            if (!read_u16(d, len, 0, key)) return std::nullopt;
            // AddKeyEvent accepts named non-mouse keys or one modifier bit,
            // never arbitrary native keycodes, mouse aliases, or chords.
            const bool named = key >= ImGuiKey_NamedKey_BEGIN && key < ImGuiKey_NamedKey_END;
            const bool mouse = key >= ImGuiKey_MouseLeft && key <= ImGuiKey_MouseWheelY;
            const bool modifier = key == ImGuiMod_Ctrl || key == ImGuiMod_Shift ||
                                  key == ImGuiMod_Alt || key == ImGuiMod_Super;
            if ((!named || mouse) && !modifier) return std::nullopt;
            msg.input.key = int32_t(key);
            return msg;
        }
        case 0x16: {  // text input
            msg.kind = ClientMsg::Kind::Input;
            msg.input.ev_type = 6;
            uint32_t ch;
            if (!read_u32(d, len, 0, ch)) return std::nullopt;
            if (ch > 0x10ffff || (ch >= 0xd800 && ch <= 0xdfff)) return std::nullopt;
            msg.input.character = ch;
            return msg;
        }
        case 0x17: {  // resize (scale is optional)
            msg.kind = ClientMsg::Kind::Resize;
            if (!read_f32(d, len, 0, msg.resize_w) || !read_f32(d, len, 4, msg.resize_h))
                return std::nullopt;
            if (!std::isfinite(msg.resize_w) || !std::isfinite(msg.resize_h) ||
                msg.resize_w <= 0 || msg.resize_h <= 0) return std::nullopt;
            // Absent or nonsensical scale stays 0 ("not provided").
            if (len >= 12) {
                if (!read_f32(d, len, 8, msg.resize_scale)) return std::nullopt;
                if (!std::isfinite(msg.resize_scale) || !(msg.resize_scale > 0.0f)) msg.resize_scale = 0.0f;
            }
            return msg;
        }
        case 0x1b: { // browser-local media metadata, never pixel/file data
            uint32_t flags, w, h, n;
            float position, duration, volume;
            if (len < 28 || !read_u32(d,len,0,flags) || flags > 31 || (flags & 3) > 2 ||
                !read_u32(d,len,4,w) || !read_u32(d,len,8,h) || w > 32768 || h > 32768 ||
                !read_f32(d,len,12,position) || !read_f32(d,len,16,duration) || !read_f32(d,len,20,volume) ||
                !std::isfinite(position) || !std::isfinite(duration) || !std::isfinite(volume) ||
                position < 0 || duration < 0 || volume < 0 || volume > 1 ||
                !read_u32(d,len,24,n) || n > 1023 || len != 28+n) return std::nullopt;
            msg.kind = ClientMsg::Kind::Media;
            auto& m=msg.media;
            m.kind=flags&3; m.paused=(flags&4)!=0; m.muted=(flags&8)!=0; m.error=(flags&16)!=0;
            m.width=w; m.height=h; m.position=position; m.duration=duration; m.volume=volume;
            std::memcpy(m.message,d+28,n); m.message[n]=0;
            return msg;
        }
        case 0x18: {  // clipboard text from client
            msg.kind = ClientMsg::Kind::ClipboardText;
            uint32_t text_len;
            if (!read_u32(d, len, 0, text_len)) return std::nullopt;
            if (size_t(4) + text_len > len) return std::nullopt;
            msg.clipboard_text.assign(reinterpret_cast<const char*>(d) + 4, text_len);
            return msg;
        }
        case 0x1d: {
            msg.kind = ClientMsg::Kind::DrawAck;
            if (len != 4 || !read_u32(d, len, 0, msg.draw_ack)) return std::nullopt;
            return msg;
        }
        case 0x1a: {  // capability ack
            msg.kind = ClientMsg::Kind::HelloAck;
            uint32_t caps;
            if (!read_u32(d, len, 0, caps)) return std::nullopt;
            msg.capabilities = caps;
            return msg;
        }
        default:
            return std::nullopt;
    }
}

}  // namespace

std::optional<std::vector<std::pair<ClientId, ClientMsg>>> parse_client_msgs(
    const uint8_t* data, size_t len) {
    std::vector<std::pair<ClientId, ClientMsg>> messages;

    if (len == 0) return std::nullopt;

    if (data[0] != 0x19) {
        // A single unbatched message.
        if (len < 5) return std::nullopt;
        uint32_t id;
        if (!read_u32(data, len, 1, id)) return std::nullopt;
        auto payload = parse_client_payload(data[0], data + 5, len - 5);
        if (!payload) return std::nullopt;
        messages.emplace_back(id, std::move(*payload));
        return messages;
    }

    // 0x19 batch envelope: records share the outer client id and contain
    // schema-sized payloads without the repeated four-byte id.
    uint32_t id;
    uint16_t count;
    if (!read_u32(data, len, 1, id) || !read_u16(data, len, 5, count)) return std::nullopt;
    size_t off = 7;
    messages.reserve(count);
    for (size_t i = 0; i < count; i++) {
        if (off + 3 > len) return std::nullopt;
        uint8_t msg_type = data[off];
        uint16_t payload_len = uint16_t(data[off + 1]) | (uint16_t(data[off + 2]) << 8);
        off += 3;
        if (off + payload_len > len) return std::nullopt;
        auto payload = parse_client_payload(msg_type, data + off, payload_len);
        if (!payload) return std::nullopt;
        off += payload_len;
        messages.emplace_back(id, std::move(*payload));
    }
    if (off != len) return std::nullopt;
    return messages;
}

std::optional<uint32_t> leading_hello_ack(
    const std::vector<std::pair<ClientId, ClientMsg>>& msgs, ClientId expected) {
    // Handshake acceptance: the capability ack must lead its input
    // record; anything else leaves the handshake window waiting.
    if (msgs.empty()) return std::nullopt;
    const auto& [cid, msg] = msgs.front();
    if (msg.kind != ClientMsg::Kind::HelloAck) return std::nullopt;
    if (cid != expected) return std::nullopt;
    return msg.capabilities;
}

std::vector<uint8_t> make_clipboard_write_msg(const std::string& text) {
    std::vector<uint8_t> msg;
    msg.reserve(1 + 4 + text.size());
    msg.push_back(0x18);
    uint32_t len = uint32_t(text.size());
    for (int i = 0; i < 4; i++) msg.push_back(uint8_t(len >> (8 * i)));
    msg.insert(msg.end(), text.begin(), text.end());
    return msg;
}

// --- State -------------------------------------------------------------------

ClientId State::add_client(uint32_t peer_addr, std::function<void()> notify) {
    ClientId id;
    std::shared_ptr<OutBox> out;
    {
        std::lock_guard<std::mutex> lk(clients_mtx_);
        id = next_client_id_++;
        auto [it, inserted] = clients_.emplace(id, ClientState{});
        it->second.peer_addr = peer_addr;
        out = it->second.out;
        out->transport_notify = std::move(notify);
    }
    {
        std::lock_guard<std::mutex> lk(active_mtx_);
        if (!active_client_.has_value()) active_client_ = id;
    }
    return id;
}

void State::remove_client(ClientId id) {
    std::shared_ptr<OutBox> out;
    {
        std::lock_guard<std::mutex> lk(clients_mtx_);
        auto it = clients_.find(id);
        if (it != clients_.end()) {
            out = it->second.out;
            auto release_owned = [&](auto& held, bool mouse) {
                for (auto press = held.begin(); press != held.end();) {
                    if (press->second != id) { ++press; continue; }
                    InputEvent release;
                    release.ev_type = mouse ? 2 : 5;
                    if (mouse) release.button = press->first;
                    else release.key = press->first;
                    disconnect_releases_.push_back(release);
                    press = held.erase(press);
                }
            };
            release_owned(held_buttons_, true);
            release_owned(held_keys_, false);
            clients_.erase(it);
        }
    }
    if (out) {
        out->closed.store(true, std::memory_order_release);
        out->notify();
    }
    std::lock_guard<std::mutex> lk(active_mtx_);
    if (active_client_ == id) {
        std::lock_guard<std::mutex> clk(clients_mtx_);
        active_client_ = clients_.empty() ? std::nullopt
                                          : std::optional<ClientId>(clients_.begin()->first);
    }
}

bool State::has_clients() const {
    std::lock_guard<std::mutex> lk(clients_mtx_);
    return !clients_.empty();
}

void State::set_client_limits(const ClientLimits& limits) {
    std::lock_guard<std::mutex> lk(limits_mtx_);
    limits_ = limits;
}

bool State::connection_allowed(uint32_t peer_addr) const {
    uint32_t max_total;
    uint32_t max_ip;
    {
        std::lock_guard<std::mutex> lk(limits_mtx_);
        max_total = limits_.max_clients;
        max_ip = limits_.max_clients_per_ip;
    }
    if (max_total == 0 && max_ip == 0) return true;
    std::lock_guard<std::mutex> lk(clients_mtx_);
    if (max_total != 0 && clients_.size() >= max_total) return false;
    if (max_ip != 0) {
        size_t from_peer = 0;
        for (const auto& [id, cs] : clients_) {
            if (cs.peer_addr == peer_addr) from_peer++;
        }
        if (from_peer >= max_ip) return false;
    }
    return true;
}

void State::push_input(ClientId id, const InputEvent& ev) {
    uint64_t seq = next_input_seq_.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(clients_mtx_);
        auto it = clients_.find(id);
        // Events for a vanished client are dropped rather than parked in a
        // global queue someone else would have to drain.
        if (it != clients_.end()) it->second.input.emplace_back(seq, ev);
    }
    std::lock_guard<std::mutex> lk(active_mtx_);
    active_client_ = id;
}

void State::prepare_input_frame(float size[2], float& scale, bool keep_layout) {
    std::lock_guard<std::mutex> active_lock(active_mtx_);
    std::lock_guard<std::mutex> clients_lock(clients_mtx_);
    input_pointer_moved_this_frame_ = false;
    // Do not append fresh edges while ImGui is still trickling the prior
    // batch. Otherwise a deferred move can meet a newly submitted release
    // in the same ImGui frame, bypassing the hover preparation barrier.
    input_frame_limit_ = keep_layout ? 0 : next_input_seq_.load(std::memory_order_relaxed) - 1;
    if (!keep_layout || !input_frame_prepared_) {
        auto owner = clients_.end();
        // Oldest pending work owns the next layout. Newer arrivals cannot
        // strand it by continually reclaiming the viewport.
        for (auto it = clients_.begin(); it != clients_.end(); ++it) {
            if (it->second.input.empty()) continue;
            if (owner == clients_.end() || it->second.input.front().first < owner->second.input.front().first)
                owner = it;
        }
        if (owner == clients_.end() && active_client_) owner = clients_.find(*active_client_);
        if (owner != clients_.end()) {
            input_frame_size_[0] = owner->second.display_size[0];
            input_frame_size_[1] = owner->second.display_size[1];
            input_frame_scale_ = owner->second.display_scale;
            active_client_ = owner->first;
        }
    }
    input_frame_prepared_ = true;
    size[0] = input_frame_size_[0];
    size[1] = input_frame_size_[1];
    scale = input_frame_scale_;
}

std::optional<InputEvent> State::try_poll_input() {
    std::lock_guard<std::mutex> active_lock(active_mtx_);
    std::lock_guard<std::mutex> lk(clients_mtx_);
    // Releases of disconnected owners do not need hit-test geometry. Drain
    // them before newly queued presses so cleanup cannot release a new owner.
    if (!disconnect_releases_.empty()) {
        InputEvent release = disconnect_releases_.front();
        disconnect_releases_.pop_front();
        return release;
    }
    auto best = clients_.end();
    for (auto it = clients_.begin(); it != clients_.end(); ++it) {
        auto& q = it->second.input;
        if (q.empty()) continue;
        if (best == clients_.end() || q.front().first < best->second.input.front().first) best = it;
    }
    if (best == clients_.end()) return std::nullopt;
    auto& cs = best->second;
    if (input_frame_prepared_ && cs.input.front().first > input_frame_limit_) return std::nullopt;
    if (cs.input.front().second.ev_type <= 3) {
        const float* sz = cs.display_size;
        // ImGui hit-tests previous geometry and draws this frame's geometry.
        // Both must match. Stop at the FIFO head instead of letting newer
        // clients overtake work that is waiting for its preparation frame.
        if (sz[0] != layout_size_[0] || sz[1] != layout_size_[1] ||
            (input_frame_prepared_ && (sz[0] != input_frame_size_[0] || sz[1] != input_frame_size_[1])))
            return std::nullopt;
    }
    // A newly hovered item needs one submitted frame before button edges.
    // Tabs use AllowOverlap and reject a click when HoveredIdPreviousFrame
    // differs; drag targets also need a preview frame before delivery.
    const int kind = cs.input.front().second.ev_type;
    if (input_frame_prepared_ && input_pointer_moved_this_frame_ && (kind == 1 || kind == 2))
        return std::nullopt;
    InputEvent out = cs.input.front().second;
    if (kind == 0) {
        input_pointer_moved_this_frame_ |= !input_pointer_valid_ || out.x != input_pointer_x_ || out.y != input_pointer_y_;
        input_pointer_x_ = out.x;
        input_pointer_y_ = out.y;
        input_pointer_valid_ = true;
    }
    out.display_w = cs.display_size[0];
    out.display_h = cs.display_size[1];
    cs.input.pop_front();
    switch (out.ev_type) {
        case 1: held_buttons_[out.button] = best->first; break;
        case 2: held_buttons_.erase(out.button); break;
        case 4: held_keys_[out.key] = best->first; break;
        case 5: held_keys_.erase(out.key); break;
        default: break;
    }
    // Clipboard ownership follows the consumed event. Frame geometry is
    // already frozen independently by prepare_input_frame.
    active_client_ = best->first;
    return out;
}

void State::set_client_capabilities(ClientId id, uint32_t capabilities) {
    std::lock_guard<std::mutex> lk(clients_mtx_);
    auto it = clients_.find(id);
    if (it != clients_.end()) {
        auto& cs = it->second;
        cs.capabilities = capabilities;

    }
}

void State::set_display_size(ClientId id, float w, float h, float scale) {
    {
        std::lock_guard<std::mutex> lk(clients_mtx_);
        auto it = clients_.find(id);
        if (it == clients_.end()) return;
        it->second.display_size[0] = w;
        it->second.display_size[1] = h;
        if (scale > 0.0f) it->second.display_scale = scale;
    }
    // Select the resized client before its first pointer event. This
    // prevents a click from changing the shared viewport mid-frame.
    std::lock_guard<std::mutex> lk(active_mtx_);
    active_client_ = id;
}

void State::get_display_size(float out[2]) const {
    std::optional<ClientId> id;
    {
        std::lock_guard<std::mutex> lk(active_mtx_);
        id = active_client_;
    }
    if (!id.has_value()) {
        out[0] = 1280.0f;
        out[1] = 720.0f;
        return;
    }
    std::lock_guard<std::mutex> lk(clients_mtx_);
    auto it = clients_.find(*id);
    if (it != clients_.end()) {
        out[0] = it->second.display_size[0];
        out[1] = it->second.display_size[1];
    } else {
        out[0] = 1280.0f;
        out[1] = 720.0f;
    }
}

float State::get_display_scale() const {
    std::optional<ClientId> id;
    {
        std::lock_guard<std::mutex> lk(active_mtx_);
        id = active_client_;
    }
    if (!id.has_value()) return 1.0f;
    std::lock_guard<std::mutex> lk(clients_mtx_);
    auto it = clients_.find(*id);
    return it != clients_.end() ? it->second.display_scale : 1.0f;
}

void State::set_clipboard_text(const std::string& text) {
    std::optional<ClientId> id;
    {
        std::lock_guard<std::mutex> lk(active_mtx_);
        id = active_client_;
    }
    if (!id.has_value()) return;
    {
        std::lock_guard<std::mutex> lk(clients_mtx_);
        auto it = clients_.find(*id);
        if (it != clients_.end()) it->second.clipboard_text = text;
    }
    send_control_to(*id, make_clipboard_write_msg(text));
}

std::string State::get_clipboard_text() const {
    std::optional<ClientId> id;
    {
        std::lock_guard<std::mutex> lk(active_mtx_);
        id = active_client_;
    }
    if (!id.has_value()) return std::string();
    std::lock_guard<std::mutex> lk(clients_mtx_);
    auto it = clients_.find(*id);
    return it != clients_.end() ? it->second.clipboard_text : std::string();
}

void State::on_clipboard_text(ClientId id, const std::string& text) {
    std::lock_guard<std::mutex> lk(clients_mtx_);
    auto it = clients_.find(id);
    if (it != clients_.end()) it->second.clipboard_text = text;
}

void State::send_control(const std::vector<uint8_t>& data) {
    std::vector<std::shared_ptr<OutBox>> outs;
    {
        std::lock_guard<std::mutex> lk(clients_mtx_);
        outs.reserve(clients_.size());
        for (auto& [id, cs] : clients_) outs.push_back(cs.out);
    }
    for (auto& out : outs) {
        {
            std::lock_guard<std::mutex> lk(out->mtx);
            out->control.push_back(data);
        }
        out->notify();
    }
}

void State::send_control_to(ClientId id, const std::vector<uint8_t>& data) {
    std::shared_ptr<OutBox> out;
    {
        std::lock_guard<std::mutex> lk(clients_mtx_);
        auto it = clients_.find(id);
        if (it == clients_.end()) return;
        out = it->second.out;
    }
    {
        std::lock_guard<std::mutex> lk(out->mtx);
        out->control.push_back(data);
    }
    out->notify();
}

void State::send_texture(uint64_t id, const std::vector<uint8_t>& msg) {
    {
        std::lock_guard<std::mutex> lk(textures_mtx_);
        textures_[id] = msg;
    }
    send_control(msg);
    // Pixel updates can change the image without changing any geometry.
    std::lock_guard<std::mutex> lock(clients_mtx_);
    for (auto& [client, cs] : clients_)
        if (cs.capabilities & kDrawIPCapability) cs.force_frames = 1;
}

std::vector<std::vector<uint8_t>> State::snapshot_textures() const {
    std::lock_guard<std::mutex> lk(textures_mtx_);
    std::vector<std::vector<uint8_t>> out;
    out.reserve(textures_.size());
    for (const auto& [id, data] : textures_) out.push_back(data);
    return out;
}

bool State::begin_frame(float dpx, float dpy, float dsw, float dsh, float fbsx, float fbsy) {
    // Record the layout used for subsequent input hit testing.
    if (!has_clients()) return false;
    layout_size_[0] = dsw;
    layout_size_[1] = dsh;
    return true;
}

}  // namespace imgui_quic_core

namespace imgui_quic_core {
void State::on_media(ClientId id, const imgui_quic_media_info& info) {
    std::lock_guard<std::mutex> lock(clients_mtx_);
    auto it=clients_.find(id); if(it!=clients_.end()) it->second.media=info;
}
imgui_quic_media_info State::get_media() const {
    std::lock_guard<std::mutex> active(active_mtx_);
    std::lock_guard<std::mutex> clients(clients_mtx_);
    if(!active_client_)return {};
    auto it=clients_.find(*active_client_);
    return it==clients_.end()?imgui_quic_media_info{}:it->second.media;
}
void State::control_media(int action,float value) {
    if(action<1 || action>8 || !std::isfinite(value))return;
    std::optional<ClientId> id;
    { std::lock_guard<std::mutex> lock(active_mtx_); id=active_client_; }
    if(!id)return;
    std::vector<uint8_t> data(6); data[0]=0x1c; data[1]=(uint8_t)action;
    uint32_t bits; std::memcpy(&bits,&value,4);
    for(int i=0;i<4;i++)data[2+i]=(uint8_t)(bits>>(i*8));
    send_control_to(*id,data);
}
}

namespace imgui_quic_core {
bool State::wants_draw_frames() const {
    std::lock_guard<std::mutex> lock(clients_mtx_);
    for (const auto& [id, cs] : clients_)
        if (cs.capabilities & kDrawIPCapability) return true;
    return false;
}
void State::acknowledge_draw(ClientId id, uint32_t frame_id) {
    std::lock_guard<std::mutex> lock(clients_mtx_);
    auto it=clients_.find(id);
    if(it==clients_.end() || !(it->second.capabilities & kDrawIPCapability)) return;
    auto& cs=it->second;
    if(frame_id==0) {
        cs.draw_base.reset(); cs.draw_pending.clear(); cs.force_frames=1;
        cs.draw_pending_sizes.clear(); cs.draw_pending_bytes=0;
        // Re-send prerequisites too: recovery must work after resource loss.
        const auto textures=snapshot_textures();
        std::lock_guard<std::mutex> out_lock(cs.out->mtx);
        auto& q=cs.out->control;
        q.erase(std::remove_if(q.begin(),q.end(),[](const auto& m){return !m.empty() && m[0]==0x02;}),q.end());
        for(const auto& texture:textures) q.push_back(texture);
        cs.out->frame.reset(); cs.out->sent_seq=cs.out->frame_seq;
        cs.out->notify();
        return;
    }
    auto found=std::find_if(cs.draw_pending.begin(),cs.draw_pending.end(),
        [frame_id](const auto& f){return f->id==frame_id;});
    if(found==cs.draw_pending.end()) return; // stale or forged ack
    cs.draw_base=*found;
    auto count=std::distance(cs.draw_pending.begin(),found)+1;
    for(decltype(count) i=0;i<count;++i) {
        cs.draw_pending_bytes-=cs.draw_pending_sizes.front();
        cs.draw_pending_sizes.pop_front();
    }
    cs.draw_pending.erase(cs.draw_pending.begin(),std::next(found));
    cs.draw_ack_time=std::chrono::steady_clock::now();
}
void State::publish_draw_frame(std::vector<uint8_t> bytes) {
    if(bytes.size()>kMaxDrawFrameBytes) return;
    auto snapshot=std::make_shared<DrawSnapshot>();
    if(++draw_frame_id_==0) ++draw_frame_id_;
    snapshot->id=draw_frame_id_; snapshot->bytes=std::move(bytes);
    std::lock_guard<std::mutex> lock(clients_mtx_);
    const auto now=std::chrono::steady_clock::now();
    for(auto& [id,cs]:clients_) {
        if(!(cs.capabilities & kDrawIPCapability)) continue;
        // Never queue unbounded history or an unbounded stream of snapshots
        // to a receiver that stopped acknowledging. Disconnect to recover.
        if(!cs.draw_pending.empty() && now-cs.draw_ack_time>std::chrono::seconds(cs.draw_base?10:60)) {
            cs.out->closed=true; cs.out->notify(); continue;
        }
        // Bootstrap/recovery I frames are single-flight, including while the
        // receiver downloads textures. Repeating them cannot improve freshness.
        if(!cs.draw_base && !cs.draw_pending.empty()) continue;
        if(cs.draw_pending.size()>=kDrawWindow || cs.draw_pending_bytes>=kDrawByteWindow) continue;
        auto latest=cs.draw_pending.empty()?cs.draw_base:cs.draw_pending.back();
        if(latest && latest->bytes==snapshot->bytes && cs.force_frames==0 &&
           now-cs.last_send<std::chrono::seconds(5)) continue;
        auto wire=std::make_shared<const std::vector<uint8_t>>(
            encode_draw_frame(*snapshot,cs.draw_base.get(),
                              (cs.capabilities & kDrawMotionCapability) != 0));
        if(!cs.draw_pending.empty() && wire->size()>kDrawByteWindow-cs.draw_pending_bytes) continue;
        if(cs.draw_pending.empty()) cs.draw_ack_time=now;
        cs.draw_pending_sizes.push_back(wire->size());
        cs.draw_pending_bytes+=wire->size();
        cs.draw_pending.push_back(snapshot);
        cs.last_send=now; cs.force_frames=0;
        {
            std::lock_guard<std::mutex> out_lock(cs.out->mtx);
            // All deltas use acknowledged bases, so replacing an unsent
            // frame cannot break a dependency and needs no reset.
            cs.out->frame=std::move(wire); ++cs.out->frame_seq;
        }
        cs.out->notify();
    }
}
}
