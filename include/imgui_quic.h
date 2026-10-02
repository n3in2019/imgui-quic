#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* lifecycle */

typedef struct {
    /* Application client limits: 0 = unlimited. QUIC admission is configured separately. */
    unsigned max_clients;
    unsigned max_clients_per_ip;
} imgui_quic_config_t;

int  imgui_quic_init(const imgui_quic_config_t* config);
void imgui_quic_shutdown();
void imgui_quic_new_frame();
void imgui_quic_render();

#ifdef __cplusplus
}
#endif
