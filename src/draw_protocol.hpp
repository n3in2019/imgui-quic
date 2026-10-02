#pragma once
#include "core.hpp"
#include "imgui_quic_internal.h"

namespace imgui_quic_core {
constexpr uint32_t kDrawIPCapability = 1u << 4;
constexpr uint32_t kDrawMotionCapability = 1u << 5;
constexpr size_t kMaxDrawFrameBytes = 16 * 1024 * 1024;
constexpr size_t kDrawWindow = 8;
// Small deltas retain the full frame window; bulk frames must not build a
// seconds-long reliable queue. One oversized frame can always make progress.
constexpr size_t kDrawByteWindow = 24 * 1024;
struct DrawSnapshot {
    uint32_t id = 0;
    std::vector<uint8_t> bytes;
};
// Canonical draw payload matches the renderer's 0x01 draw-data layout.
std::vector<uint8_t> draw_header(FrameHeader header);
void append_draw_list(std::vector<uint8_t>& out, const void* vertices, uint32_t nv,
                      const void* indices, uint32_t ni, int index_size,
                      const imgui_quic_draw_cmd_t* commands, uint32_t nc);
std::vector<uint8_t> encode_draw_frame(const DrawSnapshot& frame, const DrawSnapshot* base,
                                       bool allow_motion = false);
}
