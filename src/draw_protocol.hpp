#pragma once
#include "core.hpp"
#include "imgui_quic_internal.h"

namespace imgui_quic_core {
constexpr uint32_t kDrawIPCapability = 1u << 4;
constexpr uint32_t kDrawMotionCapability = 1u << 5;
constexpr uint32_t kDrawQuantizedCapability = 1u << 6;
constexpr uint32_t kDrawLz4Capability = 1u << 7;
constexpr uint32_t kDrawQuarterCapability = 1u << 8;
constexpr uint32_t kDrawIntegerCapability = 1u << 9;
constexpr uint32_t kDrawPlanarCapability = 1u << 10;
constexpr size_t kMaxDrawFrameBytes = 16 * 1024 * 1024;
constexpr size_t kDrawWindow = 8;
// Small deltas retain the full frame window; bulk frames must not build a
// seconds-long reliable queue. One oversized frame can always make progress.
constexpr size_t kDrawByteWindow = 24 * 1024;
struct DrawSnapshot {
    uint32_t id = 0;
    std::vector<uint8_t> bytes;
};
// 16/4/1 position steps per unit: 0x21/0x24/0x25. Unsafe/larger frames stay exact.
std::vector<uint8_t> quantize_draw_frame(const std::vector<uint8_t>& bytes, unsigned scale = 16);
// Canonical draw payload matches the renderer's 0x01 draw-data layout.
std::vector<uint8_t> draw_header(FrameHeader header);
void append_draw_list(std::vector<uint8_t>& out, const void* vertices, uint32_t nv,
                      const void* indices, uint32_t ni, int index_size,
                      const imgui_quic_draw_cmd_t* commands, uint32_t nc);
std::vector<uint8_t> encode_draw_frame(const DrawSnapshot& frame, const DrawSnapshot* base,
                                       bool allow_motion = false, bool allow_lz4 = false, bool allow_planar = false);
}
