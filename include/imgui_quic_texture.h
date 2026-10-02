// Server-owned RGBA textures for ImGui::Image / ImageButton.
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Pass texture_id=0 to allocate, or a returned ID to replace its pixels.
// Pixels are tightly packed RGBA8, top row first. byte_count must equal
// width*height*4. Maximum upload: 64 MiB. Returns 0 on invalid input or when
// the server is not initialized. Call after init and before drawing the image.
// Data is copied, broadcast to connected browsers, and retained for late
// joiners/reconnects until server shutdown. IDs are valid for this session.
uint64_t imgui_quic_texture_upload_rgba(uint64_t texture_id, uint32_t width,
    uint32_t height, const uint8_t* pixels, size_t byte_count);
#ifdef __cplusplus
}
#endif
