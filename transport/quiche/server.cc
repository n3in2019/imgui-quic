// Google QUICHE WebTransport endpoint. All QUICHE objects stay on one I/O thread.
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <deque>
#include <fstream>
#include <future>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <utility>

#include "absl/time/time.h"
#include "bridge.h"
#include "google/protobuf/json/json.h"
#include "google/protobuf/struct.pb.h"
#include "openssl/mem.h"
#include "protocol.h"
#include "quiche/quic/core/crypto/certificate_view.h"
#include "quiche/quic/core/crypto/proof_source_x509.h"
#include "quiche/quic/core/http/web_transport_http3.h"
#include "quiche/quic/core/quic_default_connection_helper.h"
#include "quiche/quic/tools/quic_server.h"
#include "quiche/quic/tools/quic_simple_crypto_server_stream_helper.h"
#include "quiche/quic/tools/quic_simple_dispatcher.h"
#include "quiche/quic/tools/web_transport_only_backend.h"

namespace imgw {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
struct Peer;
struct Settings {
    QuicCallbacks cb;
    std::string token;
    std::set<std::string> origins;
    uint32_t max_clients, max_ip;
};
struct Runtime {
    Settings settings;
    std::atomic<bool> stop{false};
    std::thread thread;
    std::vector<std::weak_ptr<Peer>> peers;
};
struct Peer : std::enable_shared_from_this<Peer> {
    Runtime& runtime;
    webtransport::Session* session;
    quic::QuicConnection* connection;
    std::optional<webtransport::StreamId> stream_id;
    uint32_t ip, id = 0, control = 0, control_ack = 0, epoch = 1, latest_ack = 0;
    size_t datagram_limit = 0, write_offset = 0;
    bool closed = false, negotiated = false;
    Pointer pointer;
    std::string read_buffer;
    std::deque<std::string> writes, pending;
    std::map<uint32_t, bool> sent;
    Clock::time_point started = Clock::now(), progress = started, control_time = started,
                      recovery = started - 2s;
    Peer(Runtime& r, webtransport::Session* s, quic::QuicConnection* c, uint32_t address)
        : runtime(r), session(s), connection(c), ip(address) {}
    ~Peer() { disconnect(); }
    void disconnect() {
        if (id) {
            runtime.settings.cb.disconnect(runtime.settings.cb.context, id);
            id = 0;
        }
    }
    void close() {
        if (closed) return;
        closed = true;
        disconnect();
        // One application session per connection: release QUIC stream state and
        // admission immediately on malformed input, rather than waiting for idle expiry.
        if (session)
            connection->CloseConnection(
                quic::QUIC_CONNECTION_CANCELLED, "application session ended",
                quic::ConnectionCloseBehavior::SEND_CONNECTION_CLOSE_PACKET);
    }
    template <class F>
    void guard(F&& f) {
        if (closed) return;
        try {
            f();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[imgui_quic] QUIC session ended: %s\n", e.what());
            close();
        }
    }
    void input(std::string_view m) {
        const auto& cb = runtime.settings.cb;
        if (m.size() < 5 || word(m, 1) != id ||
            !cb.input(cb.context, id, reinterpret_cast<const uint8_t*>(m.data()), m.size()))
            throw std::runtime_error("native input rejected");
    }
    void queue(std::string_view m) {
        if (writes.size() >= 16 || m.empty() || m.size() > kMaxRecord)
            throw std::runtime_error("output limit");
        std::string record;
        put(record, m.size());
        record.append(m);
        writes.push_back(std::move(record));
    }
    void flush() {
        if (!session || !stream_id) return;
        auto* stream = session->GetStreamById(*stream_id);
        if (!stream) throw std::runtime_error("stream closed");
        // Small writes respect QUICHE's buffered-send high-water mark even for 64 MiB textures.
        size_t budget = 256 * 1024;
        while (!writes.empty() && budget && stream->CanWrite()) {
            auto& m = writes.front();
            size_t n = std::min({size_t(16384), m.size() - write_offset, budget});
            if (!stream->Write(absl::string_view(m.data() + write_offset, n))) break;
            write_offset += n;
            budget -= n;
            if (write_offset == m.size()) {
                writes.pop_front();
                write_offset = 0;
            }
        }
    }
    void authenticate(std::string_view b) {
        google::protobuf::Struct auth;
        if (!google::protobuf::json::JsonStringToMessage(std::string(b), &auth).ok())
            throw std::runtime_error("auth JSON");
        const auto& fields = auth.fields();
        auto v = fields.find("version"), t = fields.find("token"),
             d = fields.find("maxDatagramSize");
        const auto& token = runtime.settings.token;
        if (v == fields.end() || v->second.kind_case() != google::protobuf::Value::kNumberValue ||
            v->second.number_value() != 1 || t == fields.end() ||
            t->second.kind_case() != google::protobuf::Value::kStringValue ||
            t->second.string_value().size() != token.size() ||
            CRYPTO_memcmp(t->second.string_value().data(), token.data(), token.size()))
            throw std::runtime_error("authentication");
        if (d != fields.end() && d->second.kind_case() == google::protobuf::Value::kNumberValue) {
            double n = d->second.number_value();
            if (std::isfinite(n) && n >= 0)
                datagram_limit =
                    std::min<size_t>(std::min(n, 1100.0), session->GetMaxDatagramSize());
        }
        auto& cb = runtime.settings.cb;
        id = cb.connect(cb.context, ip);
        if (!id) throw std::runtime_error("native admission");
        started = Clock::now();
        std::string assign(1, 6);
        put(assign, id);
        pending.push_back(std::move(assign));
        std::string hello("\x0aIMGW", 5);
        put(hello, 49);
        pending.push_back(std::move(hello));
        session->SetDatagramMaxTimeInQueue(absl::Milliseconds(50));
    }
    void reset() {
        if (Clock::now() - recovery < 1s) return;
        recovery = progress = Clock::now();
        epoch = next(epoch);
        sent.clear();
        std::string m(1, 6);
        put(m, epoch);
        queue(m);
        std::string ack(1, 29);
        put(ack, id);
        put(ack, 0);
        input(ack);
    }
    void record(std::string_view b) {
        if (!id) {
            authenticate(b);
            return;
        }
        switch (uint8_t(b[0])) {
            case 3:
                for (const auto& m : pointer.reliable(b)) {
                    input(m);
                    if (m[0] == 26) negotiated = true;
                }
                break;
            case 5:
                if (b.size() != 5) throw std::runtime_error("control ACK size");
                if (auto n = word(b, 1); n >= control_ack && n <= control) control_ack = n;
                break;
            case 7: {
                if (b.size() != 9) throw std::runtime_error("draw ACK size");
                if (word(b, 1) != epoch) break;
                auto frame = word(b, 5);
                if (!frame) {
                    reset();
                    break;
                }
                if (frame > latest_ack && sent.count(frame)) {
                    latest_ack = frame;
                    sent.erase(sent.begin(), sent.upper_bound(frame));
                    progress = Clock::now();
                    std::string ack(1, 29);
                    put(ack, id);
                    put(ack, frame);
                    input(ack);
                }
                break;
            }
            default:
                throw std::runtime_error("record kind");
        }
    }
    void read() {
        if (!session || !stream_id) return;
        auto* stream = session->GetStreamById(*stream_id);
        if (!stream) throw std::runtime_error("stream closed");
        size_t budget = 256 * 1024, records = 0;
        while (budget && records < 32) {
            size_t required = 4;
            if (read_buffer.size() >= 4) {
                size_t n = word(read_buffer, 0);
                if (!n || n > (id ? kMaxRecord : 4096)) throw std::runtime_error("record size");
                required += n;
            }
            size_t n = std::min({required - read_buffer.size(), stream->ReadableBytes(), budget});
            if (n) {
                size_t offset = read_buffer.size();
                read_buffer.resize(offset + n);
                auto result = stream->Read(absl::Span<char>(read_buffer.data() + offset, n));
                read_buffer.resize(offset + result.bytes_read);
                budget -= result.bytes_read;
                if (result.fin) throw std::runtime_error("input ended");
                if (!result.bytes_read) break;
            } else if (read_buffer.size() != required) {
                if (stream->PeekNextReadableRegion().fin_next)
                    throw std::runtime_error("input ended");
                break;
            }
            if (required > 4 && read_buffer.size() == required) {
                record(std::string_view(read_buffer).substr(4));
                read_buffer.clear();
                ++records;
            }
        }
    }
    void datagram(std::string_view b) {
        if (!id || b.size() != 22 || b[0] != 4) return;
        for (const auto& m : pointer.motion(word(b, 1), word(b, 5), b.substr(9))) input(m);
    }
    void queue_native(std::string_view m, bool frame) {
        const size_t size = m.size() + (frame ? 9 : 5);
        if (writes.size() >= 16 || size > kMaxRecord) throw std::runtime_error("output limit");
        // One allocation and one payload copy, including the stream record prefix.
        std::string record;
        record.reserve(4 + size);
        put(record, size);
        record.push_back(frame ? 2 : 1);
        if (frame) put(record, epoch);
        put(record, control);
        record.append(m);
        writes.push_back(std::move(record));
    }
    void send_native(std::string_view m) {
        if (uint8_t(m[0]) >= 13 && uint8_t(m[0]) <= 15) {
            const auto frame = word(m, 1);
            if (frame <= latest_ack || sent.size() >= 8)
                throw std::runtime_error("frame window/wrap");
            bool reliable = m[0] == 13 || m.size() + 9 > datagram_limit;
            if (!reliable) {
                std::string packet;
                packet.reserve(9 + m.size());
                packet.push_back(2);
                put(packet, epoch);
                put(packet, control);
                packet.append(m);
                auto status = session->SendOrQueueDatagram(packet);
                if (status.code == webtransport::DatagramStatusCode::kTooBig)
                    reliable = true;
                else if (status.code == webtransport::DatagramStatusCode::kInternalError)
                    throw std::runtime_error("datagram send");
                // A blocked datagram is dropped; acknowledged-baseline recovery handles it.
            }
            if (sent.empty()) progress = Clock::now();
            sent.emplace(frame, reliable);
            if (reliable) queue_native(m, true);
        } else {
            control = next(control);
            control_time = Clock::now();
            queue_native(m, false);
        }
    }
    bool has_immediate_work() const {
        if (closed || !session || !stream_id) return false;
        auto* stream = session->GetStreamById(*stream_id);
        // Continue budget-limited reads/writes without relying on another UDP packet.
        return stream && (stream->ReadableBytes() || (!writes.empty() && stream->CanWrite()));
    }
    void tick() {
        guard([&] {
            if (!id && Clock::now() - started > 5s)
                throw std::runtime_error("authentication timeout");
            read();
            flush();
            if (!id) return;
            if (!negotiated && Clock::now() - started > 15s)
                throw std::runtime_error("capability timeout");
            if (control != control_ack && Clock::now() - control_time > 60s)
                throw std::runtime_error("control ACK timeout");
            if (!sent.empty() &&
                std::none_of(sent.begin(), sent.end(), [](auto& item) { return item.second; }) &&
                Clock::now() - progress > 2s)
                reset();
            if (control != control_ack || !writes.empty()) return;
            if (!pending.empty()) {
                auto m = std::move(pending.front());
                pending.pop_front();
                send_native(m);
            } else {
                const uint8_t* data = nullptr;
                size_t n = 0;
                void* cookie = nullptr;
                auto& cb = runtime.settings.cb;
                const int result = cb.poll(cb.context, id, &data, &n, &cookie);
                // Keep the immutable native buffer alive through packet construction.
                std::unique_ptr<void, void (*)(void*)> owned(cookie, cb.release);
                if (result < 0) throw std::runtime_error("native session closed");
                if (result > 0) {
                    if (!data || !n || n > kMaxRecord)
                        throw std::runtime_error("native record size");
                    send_native(std::string_view(reinterpret_cast<const char*>(data), n));
                }
            }
            flush();
        });
    }
};
class StreamVisitor final : public webtransport::StreamVisitor {
    std::weak_ptr<Peer> peer_;

   public:
    explicit StreamVisitor(std::weak_ptr<Peer> p) : peer_(std::move(p)) {}
    void OnCanRead() override {
        if (auto p = peer_.lock()) p->guard([&] { p->read(); });
    }
    void OnCanWrite() override {
        if (auto p = peer_.lock()) p->guard([&] { p->flush(); });
    }
    void OnResetStreamReceived(webtransport::StreamErrorCode) override {
        if (auto p = peer_.lock()) p->close();
    }
    void OnStopSendingReceived(webtransport::StreamErrorCode) override {
        if (auto p = peer_.lock()) p->close();
    }
    void OnWriteSideInDataRecvdState() override {}
};
class SessionVisitor final : public webtransport::SessionVisitor {
    std::shared_ptr<Peer> peer_;

   public:
    explicit SessionVisitor(std::shared_ptr<Peer> p) : peer_(std::move(p)) {}
    ~SessionVisitor() override {
        peer_->closed = true;
        peer_->session = nullptr;
        peer_->disconnect();
    }
    void OnSessionReady() override {}
    void OnSessionClosed(webtransport::SessionErrorCode, const std::string&) override {
        peer_->closed = true;
        peer_->disconnect();
    }
    void OnIncomingBidirectionalStreamAvailable() override {
        auto p = peer_;
        p->guard([&] {
            while (auto* s = p->session->AcceptIncomingBidirectionalStream()) {
                if (p->stream_id) throw std::runtime_error("unexpected stream");
                p->stream_id = s->GetStreamId();
                s->SetVisitor(std::make_unique<StreamVisitor>(p));
                p->read();
            }
        });
    }
    void OnIncomingUnidirectionalStreamAvailable() override { peer_->close(); }
    void OnDatagramReceived(absl::string_view b) override {
        auto p = peer_;
        p->guard([&] { p->datagram(b); });
    }
    void OnCanCreateNewOutgoingBidirectionalStream() override {}
    void OnCanCreateNewOutgoingUnidirectionalStream() override {}
};
class Dispatcher final : public quic::QuicSimpleDispatcher {
   public:
    using QuicSimpleDispatcher::QuicSimpleDispatcher;
    uint32_t max_clients = 8, max_ip = 2;

   protected:
    QuicPacketFate ValidityChecksOnFullChlo(const quic::ReceivedPacketInfo& packet,
                                            const quic::ParsedClientHello&) const override {
        const auto sessions = GetSessionsSnapshot();
        size_t count = 0;
        for (const auto& s : sessions)
            if (s->connection()->peer_address().host() == packet.peer_address.host()) ++count;
        return sessions.size() >= max_clients || count >= max_ip ||
                       !packet.peer_address.host().IsIPv4()
                   ? kFateTimeWait
                   : kFateProcess;
    }
};
class Server final : public quic::QuicServer {
    Settings& settings_;
    std::map<quic::QuicSession*, Clock::time_point> connected_;

   public:
    Server(std::unique_ptr<quic::ProofSource> proof, const quic::QuicConfig& config,
           quic::QuicSimpleServerBackend* backend, Settings& settings)
        : QuicServer(std::move(proof), nullptr, config, {}, quic::CurrentSupportedVersions(),
                     backend, quic::kQuicDefaultConnectionIdLength),
          settings_(settings) {}
    auto sessions() { return dispatcher()->GetSessionsSnapshot(); }
    void expire_unopened_sessions(Runtime& runtime) {
        const auto current = sessions();
        std::set<quic::QuicSession*> live;
        for (const auto& connection : current) {
            live.insert(connection.get());
            auto [entry, inserted] = connected_.emplace(connection.get(), Clock::now());
            if (Clock::now() - entry->second <= 5s) continue;
            bool active = false;
            for (auto& weak : runtime.peers)
                if (auto p = weak.lock(); p && !p->closed && p->session) {
                    auto* stream = connection->GetActiveStream(
                        static_cast<quic::WebTransportHttp3*>(p->session)->id());
                    if (stream &&
                        static_cast<quic::QuicSpdyStream*>(stream)->web_transport() == p->session)
                        active = true;
                }
            if (!active)
                connection->connection()->CloseConnection(
                    quic::QUIC_CONNECTION_CANCELLED, "WebTransport session timeout",
                    quic::ConnectionCloseBehavior::SEND_CONNECTION_CLOSE_PACKET);
        }
        for (auto i = connected_.begin(); i != connected_.end();) {
            if (!live.count(i->first))
                i = connected_.erase(i);
            else
                ++i;
        }
    }

   protected:
    quic::QuicDispatcher* CreateQuicDispatcher() override {
        auto* d = new Dispatcher(&config(), &crypto_config(), version_manager(),
                                 std::make_unique<quic::QuicDefaultConnectionHelper>(),
                                 std::make_unique<quic::QuicSimpleCryptoServerStreamHelper>(),
                                 event_loop()->CreateAlarmFactory(), server_backend(),
                                 expected_server_connection_id_length(), connection_id_generator());
        d->max_clients = settings_.max_clients;
        d->max_ip = settings_.max_ip;
        return d;
    }
};
class Backend final : public quic::WebTransportOnlyBackend {
    Runtime& runtime_;

   public:
    Server* server = nullptr;
    explicit Backend(Runtime& r)
        : WebTransportOnlyBackend(
              [](absl::string_view, webtransport::Session*)
                  -> absl::StatusOr<std::unique_ptr<webtransport::SessionVisitor>> {
                  return absl::PermissionDeniedError("unsupported");
              }),
          runtime_(r) {}
    WebTransportResponse ProcessWebTransportRequest(const quiche::HttpHeaderBlock& headers,
                                                    webtransport::Session* session) override {
        WebTransportResponse response;
        response.response_headers[":status"] = "403";
        auto path = headers.find(":path"), origin = headers.find("origin");
        if (path == headers.end() || path->second != "/wt" || origin == headers.end() ||
            !runtime_.settings.origins.count(std::string(origin->second)))
            return response;
        // The backend is called for HTTP/3 WebTransport only. Locate its owning connection
        // through the CONNECT stream, so admission uses the real peer, never a header.
        const auto stream_id = static_cast<quic::WebTransportHttp3*>(session)->id();
        for (const auto& connection : server->sessions()) {
            auto* stream = connection->GetActiveStream(stream_id);
            if (!stream || static_cast<quic::QuicSpdyStream*>(stream)->web_transport() != session)
                continue;
            const auto ip = connection->connection()->peer_address().host();
            if (!ip.IsIPv4()) return response;
            size_t total = 0, same = 0;
            uint32_t address = 0;
            auto packed = ip.ToPackedString();
            std::memcpy(&address, packed.data(), 4);
            for (auto& weak : runtime_.peers)
                if (auto p = weak.lock(); p && !p->closed) {
                    if (p->connection == connection->connection()) {
                        response.response_headers[":status"] = "429";
                        return response;
                    }
                    ++total;
                    if (p->ip == address) ++same;
                }
            if (total >= runtime_.settings.max_clients || same >= runtime_.settings.max_ip) {
                response.response_headers[":status"] = "429";
                return response;
            }
            auto peer =
                std::make_shared<Peer>(runtime_, session, connection->connection(), address);
            runtime_.peers.push_back(peer);
            response.response_headers[":status"] = "200";
            response.visitor = std::make_unique<SessionVisitor>(std::move(peer));
            return response;
        }
        return response;
    }
};
// eventfd bridges native mailbox publication into QUICHE's I/O loop. Notifications
// coalesce in the kernel; no periodic polling or transport API calls from render threads.
class WakeListener final : public quic::QuicSocketEventListener {
    quic::QuicEventLoop* loop_;
    int fd_;

   public:
    WakeListener(quic::QuicEventLoop* loop, int fd) : loop_(loop), fd_(fd) {
        if (!loop_->RegisterSocket(fd_, quic::kSocketEventReadable, this))
            throw std::runtime_error("QUIC wake registration");
    }
    ~WakeListener() override { (void)loop_->UnregisterSocket(fd_); }
    void OnSocketEvent(quic::QuicEventLoop*, quic::SocketFd, quic::QuicSocketEventMask) override {
        uint64_t value;
        while (::read(fd_, &value, sizeof(value)) >= 0 || errno == EINTR) {
        }
        if (!loop_->SupportsEdgeTriggered() && !loop_->RearmSocket(fd_, quic::kSocketEventReadable))
            throw std::runtime_error("QUIC wake rearm");
    }
};
std::string trim(std::string s) {
    const auto a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
}  // namespace imgw

extern "C" void* imgw_quic_start(const char* host, uint16_t port, const char* cert, const char* key,
                                 const char* token_file, const char* origins, uint32_t max_clients,
                                 uint32_t max_ip, QuicCallbacks cb) {
    using namespace imgw;
    try {
        if (!host || !cert || !key || !token_file || !origins) return nullptr;
        quic::QuicIpAddress ip;
        if (!ip.FromString(host) || !ip.IsIPv4())
            throw std::runtime_error("QUIC requires an IPv4 bind address");
        auto runtime = std::make_unique<Runtime>();
        auto& settings = runtime->settings;
        settings.cb = cb;
        settings.max_clients = std::max(1u, max_clients);
        settings.max_ip = std::max(1u, max_ip);
        std::ifstream token_input(token_file);
        settings.token = trim(std::string(std::istreambuf_iterator<char>(token_input), {}));
        if (settings.token.size() < 32 || settings.token.size() > 2048)
            throw std::runtime_error("QUIC token length must be 32..2048 bytes");
        std::string origin_list(origins);
        size_t start = 0;
        do {
            auto end = origin_list.find(',', start);
            auto o = trim(origin_list.substr(start, end - start));
            if (!o.empty()) settings.origins.insert(std::move(o));
            if (end == std::string::npos) break;
            start = end + 1;
        } while (true);
        if (settings.origins.empty()) throw std::runtime_error("QUIC requires allowed origins");
        std::ifstream cert_input(cert), key_input(key);
        auto chain = quic::CertificateView::LoadPemFromStream(&cert_input);
        auto private_key = quic::CertificatePrivateKey::LoadPemFromStream(&key_input);
        if (chain.empty() || !private_key) throw std::runtime_error("QUIC certificate/key");
        auto proof = quic::ProofSourceX509::Create(
            quiche::QuicheReferenceCountedPointer<quic::ProofSource::Chain>(
                new quic::ProofSource::Chain(chain)),
            std::move(*private_key));
        if (!proof) throw std::runtime_error("QUIC certificate/key mismatch");
        std::promise<bool> ready;
        auto result = ready.get_future();
        auto* r = runtime.get();
        runtime->thread = std::thread([r, ip, port, proof = std::move(proof),
                                       ready = std::move(ready)]() mutable {
            bool notified = false;
            try {
                Backend backend(*r);
                quic::QuicConfig config;
                config.SetMaxBidirectionalStreamsToSend(4);
                config.SetMaxUnidirectionalStreamsToSend(8);
                config.SetInitialStreamFlowControlWindowToSend(1024 * 1024);
                config.SetInitialSessionFlowControlWindowToSend(2 * 1024 * 1024);
                config.set_max_time_before_crypto_handshake(quic::QuicTime::Delta::FromSeconds(5));
                config.set_max_idle_time_before_crypto_handshake(
                    quic::QuicTime::Delta::FromSeconds(5));
                config.SetIdleNetworkTimeout(quic::QuicTime::Delta::FromSeconds(60));
                Server server(std::move(proof), config, &backend, r->settings);
                backend.server = &server;
                if (!server.CreateUDPSocketAndListen(quic::QuicSocketAddress(ip, port)))
                    throw std::runtime_error("QUIC bind failed");
                {
                    WakeListener wake(server.event_loop(), r->settings.cb.wake_fd);
                    ready.set_value(true);
                    notified = true;
                    auto next_housekeeping = Clock::now();
                    bool immediate = false;
                    while (!r->stop.load(std::memory_order_relaxed)) {
                        server.event_loop()->RunEventLoopOnce(
                            quic::QuicTime::Delta::FromMilliseconds(immediate ? 0 : 100));
                        if (Clock::now() >= next_housekeeping) {
                            server.expire_unopened_sessions(*r);
                            next_housekeeping = Clock::now() + 100ms;
                        }
                        auto peers = r->peers;  // Callbacks can change the registry.
                        immediate = false;
                        for (auto& weak : peers)
                            if (auto p = weak.lock()) {
                                p->tick();
                                immediate = immediate || p->has_immediate_work();
                            }
                        auto& all = r->peers;
                        all.erase(std::remove_if(all.begin(), all.end(),
                                                 [](auto& w) { return w.expired(); }),
                                  all.end());
                    }
                }
                server.Shutdown();
            } catch (const std::exception& e) {
                std::fprintf(stderr, "[imgui_quic] %s\n", e.what());
                if (!notified) ready.set_value(false);
            }
        });
        if (!result.get()) {
            runtime->thread.join();
            return nullptr;
        }
        return runtime.release();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[imgui_quic] %s\n", e.what());
        return nullptr;
    }
}
extern "C" void imgw_quic_stop(void* handle) {
    if (!handle) return;
    std::unique_ptr<imgw::Runtime> runtime(static_cast<imgw::Runtime*>(handle));
    runtime->stop.store(true, std::memory_order_relaxed);
    const uint64_t one = 1;
    ssize_t result;
    do {
        result = ::write(runtime->settings.cb.wake_fd, &one, sizeof(one));
    } while (result < 0 && errno == EINTR);
    runtime->thread.join();
}
