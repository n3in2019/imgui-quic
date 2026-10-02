// Application framing and pointer fences shared by the QUICHE adapter and tests.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
namespace imgw {
constexpr size_t kMaxRecord = 64 * 1024 * 1024 + 1024;
inline uint32_t word(std::string_view b, size_t i) {
    if (i > b.size() || b.size() - i < 4) throw std::runtime_error("truncated integer");
    const auto* p = reinterpret_cast<const unsigned char*>(b.data() + i);
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline void put(std::string& b, uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) b.push_back(char(n >> (8 * i)));
}
inline bool finite(std::string_view b, size_t i) {
    const uint32_t n = word(b, i);
    float f;
    std::memcpy(&f, &n, sizeof(f));
    return std::isfinite(f);
}
inline uint32_t next(uint32_t n) {
    if (n == UINT32_MAX) throw std::runtime_error("sequence wrap");
    return n + 1;
}
struct Pointer {
    struct Motion {
        uint32_t generation, sequence;
        std::string message;
    };
    uint32_t control = 0, sequence = 0;
    std::optional<Motion> future;
    std::vector<std::string> motion(uint32_t g, uint32_t s, std::string_view m) {
        if (m.size() != 13 || m[0] != 16 || !finite(m, 5) || !finite(m, 9) || g < control ||
            s <= sequence)
            return {};
        if (g > control) {
            if (uint64_t(g) <= uint64_t(control) + 1024 && (!future || s > future->sequence))
                future = Motion{g, s, std::string(m)};
            return {};
        }
        sequence = s;
        return {std::string(m)};
    }
    std::vector<std::string> reliable(std::string_view b) {
        if (b.size() < 23) throw std::runtime_error("input size");
        const uint32_t g = word(b, 1), s = word(b, 5);
        auto m = b.substr(18);
        if (g != next(control) || !finite(b, 9) || !finite(b, 13) || uint8_t(b[17]) > 1 ||
            m[0] == 16 || m[0] == 25 || m[0] == 29)
            throw std::runtime_error("input ordering");
        control = g;
        sequence = std::max(sequence, s);
        std::vector<std::string> out;
        if (b[17] == 1 && m[0] != 26) {
            std::string p(1, 16);
            p.append(m.substr(1, 4));
            p.append(b.substr(9, 8));
            out.push_back(std::move(p));
        }
        out.emplace_back(m);
        if (future && future->generation <= g) {
            auto f = std::move(*future);
            future.reset();
            for (auto& p : motion(f.generation, f.sequence, f.message)) out.push_back(std::move(p));
        }
        return out;
    }
};
}  // namespace imgw
