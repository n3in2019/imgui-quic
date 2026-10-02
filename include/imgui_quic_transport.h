#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Start native WebTransport after imgui_quic_init, before admitting clients.
   Configuration strings are copied. Shutdown joins QUIC workers automatically.
   The endpoint owns interactive client sessions. */
typedef struct {
    const char* host; /* IPv4, NULL = 127.0.0.1 */
    uint16_t port; /* UDP, 0 = 4433 */
    const char* certificate_file;
    const char* private_key_file;
    const char* token_file; /* at least 32 bytes after trimming whitespace */
    const char* allowed_origins; /* required, comma-separated exact origins */
    unsigned max_clients; /* 0 = 8; includes handshakes */
    unsigned max_clients_per_ip; /* 0 = 2; includes handshakes */
} imgui_quic_transport_config_t;
/* 0 on success; negative on missing config, unsupported build or startup failure. */
int imgui_quic_start(const imgui_quic_transport_config_t* config);
#ifdef __cplusplus
}
#endif
