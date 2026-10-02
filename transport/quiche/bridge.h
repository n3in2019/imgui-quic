// Internal ABI: keep QUICHE and its C++20 dependencies out of the public C++17 API.
#pragma once
#include <stddef.h>
#include <stdint.h>
extern "C" {
struct QuicCallbacks {
    uintptr_t context;
    int wake_fd;  // Linux eventfd owned by the caller until stop returns.
    uint32_t (*connect)(uintptr_t, uint32_t);
    void (*disconnect)(uintptr_t, uint32_t);
    bool (*input)(uintptr_t, uint32_t, const uint8_t*, size_t);
    int (*poll)(uintptr_t, uint32_t, const uint8_t**, size_t*, void**);
    void (*release)(void*);
};
void* imgw_quic_start(const char*, uint16_t, const char*, const char*, const char*, const char*,
                      uint32_t, uint32_t, QuicCallbacks);
void imgw_quic_stop(void*);
}
