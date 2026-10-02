// protocol_tests.cpp — parse/handshake/coalescing tests for the wire input
// path, plus a coalescing test for the OutBox.

#include <arpa/inet.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
#include "imgui.h"

#include "core.hpp"

using namespace imgui_quic_core;

namespace {

// ImGuiConfigFlags_DockingEnable (1 << 6); spelled out so the protocol tests
// stay independent of imgui.h.
constexpr uint32_t kDockingEnable = 1u << 6;

void push_u32b(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; i++) v.push_back(uint8_t(x >> (8 * i)));
}
void push_u16b(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(uint8_t(x));
    v.push_back(uint8_t(x >> 8));
}
void push_f32b(std::vector<uint8_t>& v, float f) {
    uint32_t bits;
    memcpy(&bits, &f, 4);
    push_u32b(v, bits);
}

void parses_batched_pointer_click_in_order() {
    const ClientId id = 42;
    std::vector<uint8_t> batch = {0x19};
    push_u32b(batch, id);
    push_u16b(batch, 2);

    std::vector<uint8_t> position;
    push_f32b(position, 12.5f);
    push_f32b(position, 34.0f);
    batch.push_back(0x10);
    push_u16b(batch, uint16_t(position.size()));
    batch.insert(batch.end(), position.begin(), position.end());

    batch.push_back(0x11);
    push_u16b(batch, 1);
    batch.push_back(0);

    auto messages = parse_client_msgs(batch.data(), batch.size());
    assert(messages.has_value());
    assert(messages->size() == 2);
    assert((*messages)[0].first == id);
    assert((*messages)[0].second.kind == ClientMsg::Kind::Input);
    assert((*messages)[0].second.input.ev_type == 0);
    assert((*messages)[0].second.input.x == 12.5f);
    assert((*messages)[0].second.input.y == 34.0f);
    assert((*messages)[1].second.kind == ClientMsg::Kind::Input);
    assert((*messages)[1].second.input.ev_type == 1);
    assert((*messages)[1].second.input.button == 0);
}

void rejects_truncated_or_trailing_batch_data() {
    const uint8_t batch[] = {0x19, 1, 0, 0, 0, 1, 0, 0x11, 1, 0};
    assert(!parse_client_msgs(batch, sizeof(batch)).has_value());

    std::vector<uint8_t> valid(batch, batch + sizeof(batch));
    valid.push_back(0);
    valid.push_back(0xff);
    assert(!parse_client_msgs(valid.data(), valid.size()).has_value());
}

void parses_capability_ack() {
    std::vector<uint8_t> ack = {0x1a};
    push_u32b(ack, 7);
    push_u32b(ack, 0b1011);
    auto messages = parse_client_msgs(ack.data(), ack.size());
    assert(messages.has_value());
    assert((*messages).size() == 1);
    assert((*messages)[0].first == 7);
    assert((*messages)[0].second.kind == ClientMsg::Kind::HelloAck);
    assert((*messages)[0].second.capabilities == 0b1011);
}

void leading_hello_ack_accepts_envelope_and_bare_batches() {
    // 0x19 envelope: ack leads, trailing input is dropped.
    std::vector<uint8_t> env = {0x19};
    push_u32b(env, 7);
    push_u16b(env, 2);
    env.push_back(0x1a);
    push_u16b(env, 4);
    push_u32b(env, 0b1011);
    env.push_back(0x10);
    push_u16b(env, 8);
    push_f32b(env, 1.0f);
    push_f32b(env, 2.0f);
    auto msgs = parse_client_msgs(env.data(), env.size());
    assert(msgs.has_value());
    assert(msgs->size() == 2);
    assert(leading_hello_ack(*msgs, 7) == std::optional<uint32_t>(0b1011));

    // Bare ack: stray trailing bytes never become records, so the ack still
    // leads a one-record list.
    std::vector<uint8_t> bare = {0x1a};
    push_u32b(bare, 7);
    push_u32b(bare, 0b1011);
    bare.push_back(0x10);
    msgs = parse_client_msgs(bare.data(), bare.size());
    assert(msgs.has_value());
    assert(msgs->size() == 1);
    assert(leading_hello_ack(*msgs, 7) == std::optional<uint32_t>(0b1011));
}

void leading_hello_ack_rejects_non_leading_or_foreign_acks() {
    // Envelope with input before the ack: not a leading ack.
    std::vector<uint8_t> env = {0x19};
    push_u32b(env, 7);
    push_u16b(env, 2);
    env.push_back(0x10);
    push_u16b(env, 8);
    push_f32b(env, 1.0f);
    push_f32b(env, 2.0f);
    env.push_back(0x1a);
    push_u16b(env, 4);
    push_u32b(env, 1);
    auto msgs = parse_client_msgs(env.data(), env.size());
    assert(msgs.has_value());
    assert(!leading_hello_ack(*msgs, 7).has_value());

    // Foreign client id.
    std::vector<uint8_t> foreign = {0x1a};
    push_u32b(foreign, 8);
    push_u32b(foreign, 1);
    msgs = parse_client_msgs(foreign.data(), foreign.size());
    assert(msgs.has_value());
    assert(!leading_hello_ack(*msgs, 7).has_value());

    // Empty message list.
    std::vector<std::pair<ClientId, ClientMsg>> empty;
    assert(!leading_hello_ack(empty, 7).has_value());
}

void resize_selects_the_active_client() {
    State state;
    ClientId first = state.add_client();
    ClientId second = state.add_client();
    state.set_display_size(second, 800.0f, 437.0f);
    float size[2];
    state.get_display_size(size);
    assert(size[0] == 800.0f && size[1] == 437.0f);
    state.set_display_size(first, 1024.0f, 768.0f);
    state.get_display_size(size);
    assert(size[0] == 1024.0f && size[1] == 768.0f);
}

void parses_resize_with_and_without_scale() {
    const ClientId id = 42;
    // Modern resize: w, h, scale (devicePixelRatio).
    std::vector<uint8_t> modern = {0x17};
    push_u32b(modern, id);
    push_f32b(modern, 800.0f);
    push_f32b(modern, 437.0f);
    push_f32b(modern, 2.0f);
    auto messages = parse_client_msgs(modern.data(), modern.size());
    assert(messages.has_value());
    assert(messages->size() == 1);
    assert((*messages)[0].first == id);
    assert((*messages)[0].second.kind == ClientMsg::Kind::Resize);
    assert((*messages)[0].second.resize_w == 800.0f);
    assert((*messages)[0].second.resize_h == 437.0f);
    assert((*messages)[0].second.resize_scale == 2.0f);

    // 13-byte resize without optional scale: no scale field ("not provided").
    std::vector<uint8_t> unscaled = {0x17};
    push_u32b(unscaled, id);
    push_f32b(unscaled, 800.0f);
    push_f32b(unscaled, 437.0f);
    messages = parse_client_msgs(unscaled.data(), unscaled.size());
    assert(messages.has_value());
    assert(messages->size() == 1);
    assert((*messages)[0].second.resize_scale == 0.0f);
}

void resize_scale_follows_active_client() {
    State state;
    ClientId first = state.add_client();
    ClientId second = state.add_client();
    assert(state.get_display_scale() == 1.0f);  // no active client yet
    state.set_display_size(second, 800.0f, 437.0f, 2.0f);
    assert(state.get_display_scale() == 2.0f);
    // A later resize without a scale keeps the client's last known scale.
    state.set_display_size(second, 801.0f, 438.0f);
    assert(state.get_display_scale() == 2.0f);
    // The most recently active client's scale wins.
    state.set_display_size(first, 1024.0f, 768.0f, 1.5f);
    assert(state.get_display_scale() == 1.5f);
}

void clipboard_write_is_sent_only_to_active_client() {
    State state;
    ClientId first = state.add_client();
    ClientId second = state.add_client();
    state.set_display_size(second, 800.0f, 600.0f);

    std::shared_ptr<OutBox> first_out;
    std::shared_ptr<OutBox> second_out;
    state.with_clients([&](std::unordered_map<ClientId, ClientState>& clients) {
        first_out = clients[first].out;
        second_out = clients[second].out;
    });

    state.set_clipboard_text("private");

    assert(first_out->control.empty());
    assert(!second_out->control.empty());
    assert(second_out->control.front() == make_clipboard_write_msg("private"));
}





void input_preserves_cross_client_arrival_order() {
    State state;
    ClientId a = state.add_client();
    ClientId b = state.add_client();
    InputEvent ev;
    ev.ev_type = 0;
    ev.x = 1.0f;
    state.push_input(a, ev);
    ev.x = 2.0f;
    state.push_input(b, ev);
    ev.x = 3.0f;
    state.push_input(a, ev);
    auto e1 = state.try_poll_input();
    auto e2 = state.try_poll_input();
    auto e3 = state.try_poll_input();
    assert(e1.has_value() && e2.has_value() && e3.has_value());
    assert(e1->x == 1.0f && e2->x == 2.0f && e3->x == 3.0f);
    assert(!state.try_poll_input().has_value());
}




void positional_input_waits_for_matching_layout() {
    State state;
    ClientId big = state.add_client();
    ClientId small = state.add_client();
    state.set_display_size(big, 1920.0f, 1080.0f);
    state.set_display_size(small, 1280.0f, 720.0f);  // small is now active

    // Frame still laid out at big's size.
    state.begin_frame(0.0f, 0.0f, 1920.0f, 1080.0f, 1.0f, 1.0f);
    InputEvent key;
    key.ev_type = 4;
    key.key = 65;
    state.push_input(small, key);
    InputEvent click;
    click.ev_type = 1;
    click.button = 0;
    state.push_input(small, click);

    // Keys carry no geometry and flow immediately; the click behind them
    // waits for a layout computed at the sender's size.
    auto k = state.try_poll_input();
    assert(k.has_value() && k->key == 65);
    assert(!state.try_poll_input().has_value());  // click held

    // The next frame follows the active (small) client's size; the click flows.
    state.begin_frame(0.0f, 0.0f, 1280.0f, 720.0f, 1.0f, 1.0f);
    auto c = state.try_poll_input();
    assert(c.has_value() && c->ev_type == 1 && c->button == 0);

    // Same-size clients interchange: big's move flows against small's layout.
    state.set_display_size(big, 1280.0f, 720.0f);
    InputEvent move;
    move.ev_type = 0;
    move.x = 10.0f;
    move.y = 20.0f;
    state.push_input(big, move);
    auto m = state.try_poll_input();
    assert(m.has_value() && m->x == 10.0f && m->y == 20.0f);
}

void media_metadata_validation_and_isolation() {
    std::vector<uint8_t> msg{0x1b,1,0,0,0};
    push_u32b(msg,14);push_u32b(msg,320);push_u32b(msg,180);
    push_f32b(msg,1);push_f32b(msg,5);push_f32b(msg,0.5f);
    push_u32b(msg,4);msg.insert(msg.end(),{'t','e','s','t'});
    auto parsed=parse_client_msgs(msg.data(),msg.size());
    assert(parsed && parsed->size()==1);
    auto info=parsed->front().second.media;
    assert(info.kind==2&&info.paused&&info.muted&&info.duration==5);
    assert(std::string(info.message)=="test");
    auto invalid=msg;invalid[5]=255;assert(!parse_client_msgs(invalid.data(),invalid.size()));
    invalid=msg;invalid.pop_back();assert(!parse_client_msgs(invalid.data(),invalid.size()));
    invalid=msg;invalid[28]=0x7f;invalid[27]=0xc0;assert(!parse_client_msgs(invalid.data(),invalid.size()));
    State state;auto a=state.add_client(),b=state.add_client();
    state.on_media(a,info);state.set_display_size(a,800,600);
    assert(state.get_media().kind==2);
    state.set_display_size(b,800,600);assert(state.get_media().kind==0);
    state.remove_client(a);assert(state.get_media().kind==0);
}

void rejects_invalid_input_values() {
    auto accepts = [](uint8_t type, const std::vector<uint8_t>& payload) {
        std::vector<uint8_t> msg{type, 1, 0, 0, 0};
        msg.insert(msg.end(), payload.begin(), payload.end());
        return parse_client_msgs(msg.data(), msg.size()).has_value();
    };
    assert(!accepts(0x11, {255}));
    assert(!accepts(0x12, {ImGuiMouseButton_COUNT}));
    assert(accepts(0x11, {4}));
    for (unsigned key : {0u, 511u, 65535u, unsigned(ImGuiKey_MouseLeft), unsigned(ImGuiKey_MouseWheelY), unsigned(ImGuiMod_Ctrl|ImGuiMod_Shift)}) {
        std::vector<uint8_t> payload;
        push_u16b(payload, uint16_t(key));
        assert(!accepts(0x14, payload));
        assert(!accepts(0x15, payload));
    }
    for (unsigned key : {unsigned(ImGuiKey_A), unsigned(ImGuiKey_GamepadStart), unsigned(ImGuiMod_Ctrl), unsigned(ImGuiMod_Super)}) {
        std::vector<uint8_t> payload;
        push_u16b(payload, uint16_t(key));
        assert(accepts(0x14, payload));
    }
    for (float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        std::vector<uint8_t> payload;
        push_f32b(payload, bad); push_f32b(payload, 600);
        for (uint8_t type : {0x10, 0x13, 0x17}) assert(!accepts(type, payload));
    }
    for (unsigned ch : {0xd800u, 0xdfffu, 0x110000u, 0xffffffffu}) {
        std::vector<uint8_t> payload; push_u32b(payload, ch);
        assert(!accepts(0x16, payload));
    }
}

void pending_clients_receive_matching_layouts_without_starvation() {
    State state;
    auto a=state.add_client(), b=state.add_client();
    state.set_display_size(a, 800, 600);
    state.set_display_size(b, 1200, 900);
    state.begin_frame(0,0,1200,900,1,1);
    InputEvent ev; ev.ev_type=1; ev.button=0;
    state.push_input(a,ev);
    ev.button=1; state.push_input(b,ev);
    float size[2], scale;
    state.prepare_input_frame(size,scale);
    assert(size[0]==800 && !state.try_poll_input());
    state.begin_frame(0,0,size[0],size[1],scale,scale);
    state.prepare_input_frame(size,scale);
    auto first=state.try_poll_input();
    assert(first && first->button==0 && first->display_w==800);
    assert(!state.try_poll_input());
    // ImGui may still have trickled events: don't change viewport yet.
    state.prepare_input_frame(size,scale,true);
    assert(size[0]==800 && !state.try_poll_input());
    state.prepare_input_frame(size,scale);
    assert(size[0]==1200 && !state.try_poll_input());
    state.begin_frame(0,0,size[0],size[1],scale,scale);
    state.prepare_input_frame(size,scale);
    auto second=state.try_poll_input();
    assert(second && second->button==1 && second->display_w==1200);
    assert(!state.try_poll_input());
    // Arrivals during a frame wait until the next stable input snapshot.
    state.push_input(b,ev);
    assert(!state.try_poll_input());
    state.prepare_input_frame(size,scale);
    assert(state.try_poll_input());
}

void batched_click_waits_for_hover_frame() {
    State state;
    const auto client = state.add_client();
    state.set_display_size(client,800,600);
    state.begin_frame(0,0,800,600,1,1);
    InputEvent move; move.ev_type=0; move.x=195; move.y=193;
    InputEvent edge; edge.ev_type=1; edge.button=0;
    state.push_input(client,move); state.push_input(client,edge);
    edge.ev_type=2; state.push_input(client,edge);
    float size[2], scale;
    state.prepare_input_frame(size,scale);
    assert(state.try_poll_input()->ev_type==0);
    assert(!state.try_poll_input()); // move gets a full hover frame
    state.prepare_input_frame(size,scale);
    assert(state.try_poll_input()->ev_type==1);
    assert(state.try_poll_input()->ev_type==2);
    // Pending ImGui events drain before newly arrived events are appended.
    state.push_input(client,move);
    state.prepare_input_frame(size,scale,true);
    assert(!state.try_poll_input());
    state.prepare_input_frame(size,scale);
    assert(state.try_poll_input()->ev_type==0);
    // A stationary position record must not delay every click.
    state.push_input(client,move); state.push_input(client,edge);
    state.prepare_input_frame(size,scale);
    assert(state.try_poll_input()->ev_type==0);
    assert(state.try_poll_input()->ev_type==2);
    // A move followed by release needs the same barrier for drop delivery.
    move.x=300; state.push_input(client,move); state.push_input(client,edge);
    state.prepare_input_frame(size,scale);
    assert(state.try_poll_input()->ev_type==0);
    assert(!state.try_poll_input());
    state.prepare_input_frame(size,scale);
    assert(state.try_poll_input()->ev_type==2);
}

void disconnect_releases_consumed_buttons_and_keys() {
    State state;
    auto a=state.add_client(), b=state.add_client();
    InputEvent mouse; mouse.ev_type=1; mouse.button=0;
    InputEvent key; key.ev_type=4; key.key=527;
    state.push_input(a,mouse); state.push_input(a,key);
    assert(state.try_poll_input()); assert(state.try_poll_input());
    // A release queued just before disconnect must not disappear with the
    // client while leaving its consumed press held forever in ImGui.
    mouse.ev_type=2; state.push_input(a,mouse);
    state.remove_client(a);
    bool mouse_up=false,key_up=false;
    while(auto ev=state.try_poll_input()) {
        mouse_up |= ev->ev_type==2 && ev->button==0;
        key_up |= ev->ev_type==5 && ev->key==527;
    }
    assert(mouse_up && key_up);
    // Removing an old client must not release an input now owned by another.
    auto c=state.add_client(); mouse.ev_type=1;
    state.push_input(b,mouse); assert(state.try_poll_input());
    state.push_input(c,mouse); assert(state.try_poll_input());
    state.remove_client(b); assert(!state.try_poll_input());
    state.remove_client(c); auto release=state.try_poll_input();
    assert(release && release->ev_type==2 && release->button==0);
    assert(!state.try_poll_input());
}

void caps_limit_total_and_per_peer() {
    State state;
    ClientLimits limits;
    limits.max_clients = 2;
    limits.max_clients_per_ip = 1;
    state.set_client_limits(limits);
    const uint32_t peer_a = htonl(0x7f000001);
    const uint32_t peer_b = htonl(0x7f000002);
    assert(state.connection_allowed(peer_a));
    ClientId a = state.add_client(peer_a);
    assert(!state.connection_allowed(peer_a));  // per-IP cap
    assert(state.connection_allowed(peer_b));
    ClientId b = state.add_client(peer_b);
    assert(!state.connection_allowed(peer_b));  // total cap reached
    state.remove_client(a);
    assert(state.connection_allowed(peer_a));
    state.remove_client(b);
}

}  // namespace

int main() {
    batched_click_waits_for_hover_frame();
    media_metadata_validation_and_isolation();
    parses_batched_pointer_click_in_order();
    rejects_truncated_or_trailing_batch_data();
    parses_capability_ack();
    leading_hello_ack_accepts_envelope_and_bare_batches();
    leading_hello_ack_rejects_non_leading_or_foreign_acks();
    resize_selects_the_active_client();
    input_preserves_cross_client_arrival_order();
    positional_input_waits_for_matching_layout();
    parses_resize_with_and_without_scale();
    resize_scale_follows_active_client();
    clipboard_write_is_sent_only_to_active_client();
    caps_limit_total_and_per_peer();
    disconnect_releases_consumed_buttons_and_keys();
    rejects_invalid_input_values();
    pending_clients_receive_matching_layouts_without_starvation();
    printf("all core_cpp_tests passed\n");
    return 0;
}
