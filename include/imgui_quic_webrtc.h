#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Optional HTTP-compatible WebRTC backend. Start after init, before clients.
   No HTTPS certificate files. DTLS certificates are generated internally.
   Config strings are copied; shutdown closes peers and joins the worker.
   Plain HTTP/WS signaling is intended for a trusted network only. */
typedef struct {
    const char* host; /* IPv4 bind address, NULL = 127.0.0.1 */
    uint16_t signaling_port; /* Plain WebSocket TCP port, 0 = 4434 */
    const char* token_file; /* Required: 32..256 printable non-whitespace bytes */
    unsigned max_clients; /* Including unauthenticated sessions, 0 = 8 */
    unsigned max_clients_per_ip; /* 0 = 2 */
} imgui_quic_webrtc_config_t;
/* 0 on success, negative on invalid config, unsupported build or startup failure.
   WebRTC and WebTransport are mutually exclusive within one core instance. */
int imgui_quic_start_webrtc(const imgui_quic_webrtc_config_t* config);
#ifdef __cplusplus
}
#endif
