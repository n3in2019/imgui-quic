// Optional WebRTC adapter. All session/core mutations run on one worker.
// libdatachannel callbacks only append to bounded, lifetime-owned inboxes.
#include "webrtc.hpp"
#include <cstdio>
#ifdef IMGUI_QUIC_WEBRTC
#include "draw_protocol.hpp"
#include "../transport/quiche/protocol.h"
#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <arpa/inet.h>
#include <condition_variable>
#include <fstream>
#include <map>
#include <thread>

namespace imgui_quic_core {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using Json = nlohmann::json;
constexpr size_t kChunk = 16384, kBuffered = 256 * 1024, kInput = 1024 * 1024;
struct Wake { std::mutex mutex; std::condition_variable cv; };
struct Event {
    enum Kind { Signal, Answer, Channel, Reliable, Motion, Closed } kind;
    std::string bytes;
    std::shared_ptr<rtc::DataChannel> channel;
};
struct Peer : std::enable_shared_from_this<Peer> {
    std::shared_ptr<State> state;
    std::shared_ptr<Wake> wake;
    std::shared_ptr<rtc::WebSocket> ws;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> reliable, motion;
    std::mutex inbox_mutex;
    std::deque<Event> inbox;
    size_t inbox_bytes = 0;
    std::atomic<bool> overflow{false};
    uint32_t ip = 0, id = 0, control = 0, control_ack = 0, epoch = 1, latest_ack = 0;
    bool closed = false, offered = false, negotiated = false;
    imgw::Pointer pointer;
    std::string incoming;
    std::deque<std::string> writes, pending;
    size_t offset = 0;
    std::map<uint32_t, bool> sent;
    Clock::time_point started = Clock::now(), progress = started, control_time = started,
                      recovery = started - 2s;
    Peer(std::shared_ptr<State> s, std::shared_ptr<Wake> w, std::shared_ptr<rtc::WebSocket> socket)
        : state(std::move(s)), wake(std::move(w)), ws(std::move(socket)) {
        const auto address = ws->remoteAddress().value_or("");
        const auto host = address.substr(0, address.find(':'));
        inet_pton(AF_INET, host.c_str(), &ip);
    }
    void enqueue(Event event) {
        std::lock_guard<std::mutex> lock(inbox_mutex);
        if (inbox.size() >= 256 || event.bytes.size() > kInput ||
            inbox_bytes + event.bytes.size() > 2 * kInput) overflow = true;
        else { inbox_bytes += event.bytes.size(); inbox.push_back(std::move(event)); }
        wake->cv.notify_one();
    }
    void install() {
        std::weak_ptr<Peer> weak = shared_from_this();
        ws->onMessage([weak](rtc::message_variant message) {
            if (auto p = weak.lock()) {
                if (auto text = std::get_if<std::string>(&message)) p->enqueue({Event::Signal, std::move(*text), {}});
                else p->enqueue({Event::Closed, {}, {}});
            }
        });
        ws->onClosed([weak] { if (auto p = weak.lock()) p->enqueue({Event::Closed, {}, {}}); });
        ws->onError([weak](auto) { if (auto p = weak.lock()) p->enqueue({Event::Closed, {}, {}}); });
    }
    void close() {
        if (closed) return;
        closed = true;
        if (id) { state->remove_client(id); id = 0; }
        // Callbacks capture weak ownership only. Closing never waits on our worker.
        ws->resetCallbacks();
        if (pc) pc->resetCallbacks();
        for (auto ch : {reliable, motion}) if (ch) { ch->resetCallbacks(); ch->close(); }
        if (pc) pc->close();
        ws->forceClose();
    }
    ~Peer() { close(); }
    void offer(const std::string& bytes, const std::string& token, const std::string& bind) {
        if (offered || bytes.size() > 65536 || ws->path() != std::optional<std::string>("/signal"))
            throw std::runtime_error("invalid offer");
        const auto json = Json::parse(bytes, [](int depth, Json::parse_event_t, Json&) {
            if (depth > 8) throw std::runtime_error("signaling nesting limit");
            return true;
        });
        const auto supplied = json.at("token").get<std::string>();
        if (json.at("type") != "offer" || supplied.size() != token.size() ||
            CRYPTO_memcmp(supplied.data(), token.data(), token.size())) throw std::runtime_error("authentication");
        rtc::Configuration config;
        config.disableAutoNegotiation = true;
        config.bindAddress = bind;
        config.maxMessageSize = kChunk;
        pc = std::make_shared<rtc::PeerConnection>(config);
        offered = true;
        std::weak_ptr<Peer> weak = shared_from_this();
        pc->onGatheringStateChange([weak](auto status) {
            if (status == rtc::PeerConnection::GatheringState::Complete)
                if (auto p = weak.lock()) p->enqueue({Event::Answer, {}, {}});
        });
        pc->onDataChannel([weak](auto channel) {
            if (auto p = weak.lock()) p->enqueue({Event::Channel, {}, std::move(channel)});
        });
        pc->onStateChange([weak](auto status) {
            if (status == rtc::PeerConnection::State::Failed || status == rtc::PeerConnection::State::Closed ||
                status == rtc::PeerConnection::State::Disconnected)
                if (auto p = weak.lock()) p->enqueue({Event::Closed, {}, {}});
        });
        pc->setRemoteDescription(rtc::Description(json.at("sdp").get<std::string>(), "offer"));
        pc->setLocalDescription(rtc::Description::Type::Answer);
    }
    void channel(std::shared_ptr<rtc::DataChannel> ch) {
        const auto label = ch->label();
        const auto policy = ch->reliability();
        const bool durable = label == "imgui-reliable";
        if ((durable && (reliable || policy.unordered || policy.maxRetransmits || policy.maxPacketLifeTime)) ||
            (!durable && (label != "imgui-unreliable" || motion || !policy.unordered ||
                          !policy.maxRetransmits || *policy.maxRetransmits != 0 || policy.maxPacketLifeTime)))
            throw std::runtime_error("channel policy");
        (durable ? reliable : motion) = ch;
        std::weak_ptr<Peer> weak = shared_from_this();
        ch->onMessage([weak, durable](rtc::message_variant message) {
            if (auto p = weak.lock()) {
                if (auto data = std::get_if<rtc::binary>(&message)) {
                    if (data->size() > kChunk) { p->overflow = true; p->wake->cv.notify_one(); return; }
                    p->enqueue({durable ? Event::Reliable : Event::Motion,
                        std::string(reinterpret_cast<const char*>(data->data()), data->size()), {}});
                } else p->enqueue({Event::Closed, {}, {}});
            }
        });
        ch->onClosed([weak] { if (auto p = weak.lock()) p->enqueue({Event::Closed, {}, {}}); });
        ch->onError([weak](auto) { if (auto p = weak.lock()) p->enqueue({Event::Closed, {}, {}}); });
    }
    void admit() {
        if (id || !reliable || !motion || !reliable->isOpen() || !motion->isOpen()) return;
        if (!state->connection_allowed(ip)) throw std::runtime_error("core admission");
        std::weak_ptr<Wake> weak = wake;
        id = state->add_client(ip, [weak] { if (auto w = weak.lock()) w->cv.notify_one(); });
        std::string assign(1, 6); imgw::put(assign, id); pending.push_back(std::move(assign));
        std::string hello("\x0aIMGW", 5); imgw::put(hello, 2033); pending.push_back(std::move(hello));
    }
    void input(std::string_view bytes) {
        const auto msgs = parse_client_msgs(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
        if (!msgs || bytes.size() < 5 || imgw::word(bytes, 1) != id) throw std::runtime_error("input parse");
        for (const auto& [cid, message] : *msgs) if (cid != id) throw std::runtime_error("client identity");
        if (!negotiated) {
            const auto caps = leading_hello_ack(*msgs, id);
            if (!caps) return;
            if (!(*caps & kDrawIPCapability)) throw std::runtime_error("I/P required");
            state->set_client_capabilities(id, *caps);
            for (const auto& t : state->snapshot_textures()) state->send_control_to(id, t);
            negotiated = true;
            return;
        }
        for (const auto& [cid, m] : *msgs) switch (m.kind) {
            case ClientMsg::Kind::DrawAck: state->acknowledge_draw(cid, m.draw_ack); break;
            case ClientMsg::Kind::HelloAck: break;
            case ClientMsg::Kind::Input: state->push_input(cid, m.input); break;
            case ClientMsg::Kind::Resize: state->set_display_size(cid, m.resize_w, m.resize_h, m.resize_scale); break;
            case ClientMsg::Kind::Media: state->on_media(cid, m.media); break;
            case ClientMsg::Kind::ClipboardText: state->on_clipboard_text(cid, m.clipboard_text); break;
        }
    }
    void queue(std::string bytes) {
        if (writes.size() >= 16 || bytes.empty() || bytes.size() > imgw::kMaxRecord) throw std::runtime_error("output budget");
        std::string record; imgw::put(record, uint32_t(bytes.size())); record += bytes;
        writes.push_back(std::move(record));
    }
    void reset() {
        if (Clock::now() - recovery < 1s) return;
        recovery = progress = Clock::now(); epoch = imgw::next(epoch); sent.clear();
        std::string message(1, 6); imgw::put(message, epoch); queue(std::move(message));
        std::string ack(1, 29); imgw::put(ack, id); imgw::put(ack, 0); input(ack);
    }
    void record(std::string_view b) {
        if (!id || b.empty()) throw std::runtime_error("early/empty input");
        switch (uint8_t(b[0])) {
            case 3: for (const auto& m : pointer.reliable(b)) input(m); break;
            case 5:
                if (b.size() != 5) throw std::runtime_error("control ACK size");
                if (auto n = imgw::word(b, 1); n >= control_ack && n <= control) control_ack = n;
                break;
            case 7: {
                if (b.size() != 9) throw std::runtime_error("frame ACK size");
                if (imgw::word(b, 1) != epoch) break;
                const auto n = imgw::word(b, 5);
                if (!n) { reset(); break; }
                if (n > latest_ack && sent.count(n)) {
                    latest_ack = n; sent.erase(sent.begin(), sent.upper_bound(n)); progress = Clock::now();
                    std::string ack(1, 29); imgw::put(ack, id); imgw::put(ack, n); input(ack);
                }
                break;
            }
            default: throw std::runtime_error("record kind");
        }
    }
    void receive(std::string_view bytes) {
        if (incoming.size() + bytes.size() > kInput + 4) throw std::runtime_error("input budget");
        incoming += bytes;
        size_t consumed = 0;
        while (incoming.size() - consumed >= 4) {
            const auto n = imgw::word(incoming, consumed);
            if (!n || n > kInput) throw std::runtime_error("input record size");
            if (incoming.size() - consumed < n + 4) break;
            record(std::string_view(incoming).substr(consumed + 4, n)); consumed += n + 4;
        }
        incoming.erase(0, consumed);
    }
    void native(std::string_view bytes) {
        if (bytes.empty()) throw std::runtime_error("empty output");
        const auto kind = uint8_t(bytes[0]);
        const bool frame = (kind >= 13 && kind <= 15) || kind == 0x22 || kind == 0x23 || kind == 0x26 || kind == 0x27;
        std::string envelope(1, frame ? 2 : 1);
        if (frame) imgw::put(envelope, epoch);
        else { control = imgw::next(control); control_time = Clock::now(); }
        imgw::put(envelope, control); envelope.append(bytes);
        if (!frame) { queue(std::move(envelope)); return; }
        const auto n = imgw::word(bytes, 1);
        if (n <= latest_ack || sent.size() >= 8) throw std::runtime_error("frame window");
        const bool durable = kind == 13 || kind == 0x22 || kind == 0x26 || envelope.size() > 1100;
        if (sent.empty()) progress = Clock::now();
        sent.emplace(n, durable);
        if (durable) queue(std::move(envelope));
        else if (motion->bufferedAmount() < kBuffered) motion->sendBuffer(envelope);
        // Congested unreliable updates are dropped; never use them as a baseline without ACK.
    }
    void flush() {
        size_t budget = kBuffered;
        while (!writes.empty() && reliable->bufferedAmount() < kBuffered && budget) {
            const auto& data = writes.front();
            const auto n = std::min({kChunk, data.size() - offset, budget, reliable->maxMessageSize()});
            if (!n) throw std::runtime_error("SCTP message limit");
            reliable->send(reinterpret_cast<const rtc::byte*>(data.data() + offset), n);
            offset += n; budget -= n;
            if (offset == data.size()) { writes.pop_front(); offset = 0; }
        }
    }
    void tick(const std::string& token, const std::string& bind) {
        if (overflow) throw std::runtime_error("callback queue budget");
        std::deque<Event> events;
        { std::lock_guard<std::mutex> lock(inbox_mutex); events.swap(inbox); inbox_bytes = 0; }
        for (auto& event : events) {
            switch (event.kind) {
                case Event::Signal: offer(event.bytes, token, bind); break;
                case Event::Answer:
                    if (!pc || !pc->localDescription()) throw std::runtime_error("missing answer");
                    ws->send(Json{{"type", "answer"}, {"sdp", std::string(*pc->localDescription())}}.dump()); break;
                case Event::Channel: channel(std::move(event.channel)); admit(); break;
                case Event::Reliable: admit(); receive(event.bytes); break;
                case Event::Motion:
                    admit();
                    if (id && event.bytes.size() == 22 && event.bytes[0] == 4)
                        for (const auto& m : pointer.motion(imgw::word(event.bytes, 1), imgw::word(event.bytes, 5),
                                                           std::string_view(event.bytes).substr(9))) input(m);
                    break;
                case Event::Closed: close(); return;
            }
        }
        admit();
        const auto now = Clock::now();
        if ((!offered && now - started > 5s) || (!negotiated && now - started > 20s)) throw std::runtime_error("handshake timeout");
        if (!id) return;
        if (control != control_ack && now - control_time > 60s) throw std::runtime_error("control timeout");
        if (!sent.empty() && std::none_of(sent.begin(), sent.end(), [](auto& e) { return e.second; }) && now - progress > 2s) reset();
        flush();
        if (control != control_ack || !writes.empty() || reliable->bufferedAmount() >= kBuffered) return;
        if (!pending.empty()) { auto data = std::move(pending.front()); pending.pop_front(); native(data); }
        else {
            std::shared_ptr<OutBox> out;
            state->with_clients([&](auto& clients) { auto it = clients.find(id); if (it != clients.end()) out = it->second.out; });
            if (!out || out->closed) throw std::runtime_error("native session closed");
            std::shared_ptr<const std::vector<uint8_t>> data;
            { std::lock_guard<std::mutex> lock(out->mtx);
              if (!out->control.empty()) { data = std::make_shared<const std::vector<uint8_t>>(std::move(out->control.front())); out->control.pop_front(); }
              else if (out->frame_seq != out->sent_seq) { data = std::move(out->frame); out->sent_seq = out->frame_seq; }
            }
            if (data) native({reinterpret_cast<const char*>(data->data()), data->size()});
        }
        flush();
    }
};
struct Admissions {
    std::mutex mutex;
    std::deque<std::shared_ptr<rtc::WebSocket>> sockets;
    std::atomic<unsigned> count{0};
    std::atomic<bool> stopped{false};
};
} // namespace
struct WebRtcHandle {
    std::shared_ptr<State> state;
    std::shared_ptr<Wake> wake = std::make_shared<Wake>();
    std::shared_ptr<Admissions> admissions = std::make_shared<Admissions>();
    std::unique_ptr<rtc::WebSocketServer> server;
    std::thread worker;
    std::string token, bind;
    unsigned max_clients = 8, max_ip = 2;
    ~WebRtcHandle() {
        admissions->stopped = true;
        if (server) server->stop();
        wake->cv.notify_one();
        if (worker.joinable()) worker.join();
    }
    void run() {
        std::vector<std::shared_ptr<Peer>> peers;
        while (!admissions->stopped) {
            std::deque<std::shared_ptr<rtc::WebSocket>> sockets;
            { std::lock_guard<std::mutex> lock(admissions->mutex); sockets.swap(admissions->sockets); }
            for (auto& ws : sockets) {
                auto p = std::make_shared<Peer>(state, wake, std::move(ws));
                // onClient runs before the HTTP upgrade; path() is not available yet.
                // Validate the path on the first signaling message, after the upgrade.
                if (std::count_if(peers.begin(), peers.end(), [&](auto& x) { return x->ip == p->ip; }) >= max_ip) {
                    p->close(); --admissions->count; continue;
                }
                p->install(); peers.push_back(std::move(p));
            }
            for (auto& p : peers) {
                try { p->tick(token, bind); }
                catch (const std::exception&) { p->close(); } // Never log SDP or bearer tokens.
            }
            peers.erase(std::remove_if(peers.begin(), peers.end(), [&](auto& p) {
                if (!p->closed) return false; --admissions->count; return true;
            }), peers.end());
            std::unique_lock<std::mutex> lock(wake->mutex);
            wake->cv.wait_for(lock, 5ms);
        }
        for (auto& p : peers) p->close();
        std::lock_guard<std::mutex> lock(admissions->mutex);
        for (auto& ws : admissions->sockets) ws->forceClose();
        admissions->sockets.clear();
    }
};
std::shared_ptr<WebRtcHandle> start_webrtc(std::shared_ptr<State> state, const imgui_quic_webrtc_config_t& c) {
    if (state->has_clients() || !c.token_file) return nullptr;
    auto runtime = std::make_shared<WebRtcHandle>();
    runtime->state = std::move(state); runtime->bind = c.host ? c.host : "127.0.0.1";
    in_addr ip{};
    if (inet_pton(AF_INET, runtime->bind.c_str(), &ip) != 1) return nullptr;
    std::ifstream file(c.token_file, std::ios::binary);
    std::getline(file, runtime->token);
    if (!file || runtime->token.size() < 32 || runtime->token.size() > 256 ||
        std::any_of(runtime->token.begin(), runtime->token.end(), [](unsigned char ch) { return ch < 33 || ch > 126; })) return nullptr;
    runtime->max_clients = c.max_clients ? c.max_clients : 8;
    runtime->max_ip = c.max_clients_per_ip ? c.max_clients_per_ip : 2;
    rtc::WebSocketServer::Configuration config;
    config.bindAddress = runtime->bind; config.port = c.signaling_port ? c.signaling_port : 4434;
    config.maxMessageSize = 65536; config.connectionTimeout = 3s;
    runtime->server = std::make_unique<rtc::WebSocketServer>(config);
    std::weak_ptr<Admissions> weak = runtime->admissions;
    std::weak_ptr<Wake> notify = runtime->wake;
    const auto limit = runtime->max_clients;
    runtime->server->onClient([weak, notify, limit](auto ws) {
        if (auto a = weak.lock()) {
            std::lock_guard<std::mutex> lock(a->mutex);
            if (a->stopped || a->count >= limit) { ws->forceClose(); return; }
            ++a->count; a->sockets.push_back(std::move(ws));
            if (auto w = notify.lock()) w->cv.notify_one();
        } else ws->forceClose();
    });
    runtime->worker = std::thread([p = runtime.get()] { p->run(); });
    std::fprintf(stderr, "[imgui_quic] WebRTC signaling listening on ws://%s:%u/signal\n", runtime->bind.c_str(), config.port);
    return runtime;
}
} // namespace imgui_quic_core
#else
namespace imgui_quic_core {
struct WebRtcHandle {};
std::shared_ptr<WebRtcHandle> start_webrtc(std::shared_ptr<State>, const imgui_quic_webrtc_config_t&) {
    std::fprintf(stderr, "[imgui_quic] WebRTC disabled; configure IMGUI_QUIC_WEBRTC=ON\n");
    return nullptr;
}
}
#endif
