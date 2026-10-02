#include <cassert>
#include <iostream>

#include "../transport/quiche/protocol.h"
using namespace imgw;
static void number(std::string& b, float f) {
    uint32_t n;
    std::memcpy(&n, &f, 4);
    put(b, n);
}
static std::string mouse(uint32_t id, float x, float y) {
    std::string b(1, 16);
    put(b, id);
    number(b, x);
    number(b, y);
    return b;
}
int main() {
    Pointer p;
    assert(p.motion(0, 1, mouse(1, 80, 90)) == std::vector<std::string>{mouse(1, 80, 90)});
    assert(p.motion(1, 3, mouse(1, 400, 300)).empty());
    std::string edge(1, 3);
    put(edge, 1);
    put(edge, 2);
    number(edge, 100);
    number(edge, 200);
    edge.push_back(1);
    edge.push_back(18);
    put(edge, 1);
    edge.push_back(0);
    const std::vector<std::string> expected{mouse(1, 100, 200), edge.substr(18),
                                            mouse(1, 400, 300)};
    assert(p.reliable(edge) == expected);
    assert(p.motion(0, 4, mouse(1, 0, 0)).empty());
    assert(p.motion(1, 2, mouse(1, 0, 0)).empty());
    assert(p.motion(1, 5, mouse(1, std::numeric_limits<float>::quiet_NaN(), 0)).empty());
    try {
        p.reliable(edge);
        assert(false);
    } catch (const std::runtime_error&) {
    }
    Pointer bounded;
    bounded.motion(1025, 1, mouse(1, 1, 1));
    assert(!bounded.future);
    bounded.motion(1, 2, mouse(1, 2, 2));
    bounded.motion(1, 1, mouse(1, 1, 1));
    assert(bounded.future->sequence == 2);
    for (size_t n = 0; n < 23; ++n) {
        try {
            bounded.reliable(std::string(n, 0));
            assert(false);
        } catch (const std::runtime_error&) {
        }
    }
    try {
        word("abc", 0);
        assert(false);
    } catch (const std::runtime_error&) {
    }
    assert(word(std::string("\x78\x56\x34\x12", 4), 0) == 0x12345678);
    std::cout << "QUIC pointer fences, bounds and little-endian framing passed\n";
}
