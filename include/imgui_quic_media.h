// Browser-local media textures and controls. Separate from the lifecycle API.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define IMGUI_QUIC_MEDIA_TEXTURE_ID UINT64_C(4294967294)
// kind: 0 empty, 1 image, 2 video. Message is filename or decoder error.
typedef struct imgui_quic_media_info {
    uint32_t kind, width, height;
    float position, duration, volume;
    int paused, muted, error;
    char message[1024];
} imgui_quic_media_info;
enum imgui_quic_media_action {
    IMGUI_QUIC_MEDIA_PLAY = 1, IMGUI_QUIC_MEDIA_PAUSE = 2,
    IMGUI_QUIC_MEDIA_SEEK = 3, IMGUI_QUIC_MEDIA_VOLUME = 4,
    IMGUI_QUIC_MEDIA_MUTE = 5, IMGUI_QUIC_MEDIA_CLEAR = 6,
    IMGUI_QUIC_MEDIA_SAMPLE_IMAGE = 7, IMGUI_QUIC_MEDIA_SAMPLE_VIDEO = 8
};
// Operates on the currently active browser. File bytes remain in that browser.
void imgui_quic_media_get(imgui_quic_media_info* info);
void imgui_quic_media_control(int action, float value);
#ifdef __cplusplus
}
#endif
