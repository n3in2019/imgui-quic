#include "draw_protocol.hpp"
#include "imgui.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>

namespace imgui_quic_scalar_reference {
std::vector<uint8_t> quantize_draw_frame(const std::vector<uint8_t>&, unsigned);
}
using namespace imgui_quic_core;

static std::vector<uint8_t> frame(const std::vector<ImDrawVert>& vertices) {
    auto bytes=draw_header({0,0,1280,720,1,1});
    append_draw_list(bytes,vertices.data(),uint32_t(vertices.size()),nullptr,0,4,nullptr,0);
    return bytes;
}
static std::vector<uint8_t> check(const std::vector<ImDrawVert>& vertices,unsigned scale) {
    const auto bytes=frame(vertices);
    const auto packed=quantize_draw_frame(bytes,scale);
    assert(packed==imgui_quic_scalar_reference::quantize_draw_frame(bytes,scale));
    return packed;
}
static int16_t signed_word(const std::vector<uint8_t>& bytes,size_t at) {
    const uint16_t bits=uint16_t(bytes[at])|(uint16_t(bytes[at+1])<<8);
    int16_t result;std::memcpy(&result,&bits,2);return result;
}
int main() {
    std::vector<ImDrawVert> vertices(2);
    vertices[0]={{0,0},{0,1},0x11223344};
    vertices[1]={{0,0},{1,0},0x55667788};
    for(unsigned scale:{1,4,16}) {
        // Ties round away from zero on both axes; UV endpoints and colors survive.
        for(int n:{-9,-2,-1,0,1,8}) {
            const float tie=(float(n)+0.5f)/scale;
            const int expected=n<0?n:n+1;
            vertices[1].pos={tie,-tie};
            auto packed=check(vertices,scale);
            assert(packed[0]!=(uint8_t)1);
            assert(signed_word(packed,61)==expected);
            assert(signed_word(packed,63)==-expected);
            for(float x:{std::nextafter(tie,-INFINITY),std::nextafter(tie,INFINITY)}) {
                vertices[1].pos={x,-x};check(vertices,scale);
            }
        }
        // Integer boundaries and conversion guards, including adjacent floats.
        for(float x:{-32768.0f/scale,32767.0f/scale,
                     std::nextafter(-32768.5f/scale,0.0f),
                     std::nextafter(32767.5f/scale,0.0f)}) {
            vertices[1].pos={x,0};assert(check(vertices,scale)[0]!=1);
        }
        for(float x:{-32768.5f/scale,32767.5f/scale,-32769.0f/scale,32768.0f/scale,
                     std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity(),
                     std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::max()}) {
            vertices[1].pos={x,0};assert(check(vertices,scale)==frame(vertices));
        }
        vertices[1].pos={-0.0f,0};check(vertices,scale);
        for(float uv:{-0.0001f,1.0001f,INFINITY,-INFINITY,std::numeric_limits<float>::quiet_NaN()}) {
            vertices[1].uv.x=uv;assert(check(vertices,scale)==frame(vertices));
        }
        vertices[1].uv.x=1;
        // Float reconstruction near very large origins can require fallback.
        for(float origin:{-0.0f,1e-30f,-1e30f,1e30f}) {
            vertices[0].pos={origin,origin};vertices[1].pos={origin,origin};check(vertices,scale);
        }
        vertices[0].pos={0,0};
    }
    assert(quantize_draw_frame(frame(vertices),2)==frame(vertices));
    assert(quantize_draw_frame({},1).empty());
    check({},1);

    std::mt19937 rng(71869);
    size_t packed_count=0,fallback_count=0;
    for(int trial=0;trial<100000;++trial) {
        std::vector<ImDrawVert> random_vertices(2+rng()%32);
        float ox=float(int(rng()%100000)-50000)/16,oy=float(int(rng()%100000)-50000)/16;
        if(trial%10==0)ox=std::ldexp(float(int(rng()%1000)-500),int(rng()%120)-60);
        for(auto& vertex:random_vertices) {
            vertex.pos={ox+float(int(rng()%1048600)-524300)/16,oy+float(int(rng()%1048600)-524300)/16};
            vertex.uv={float(rng()%65536)/65535,float(rng()%65536)/65535};vertex.col=rng();
        }
        random_vertices[0].pos={ox,oy};
        if(trial%7==0){uint32_t bits=rng();std::memcpy(&random_vertices.back().pos.x,&bits,4);}
        if(trial%11==0){uint32_t bits=rng();std::memcpy(&random_vertices.back().uv.x,&bits,4);}
        const auto bytes=frame(random_vertices);
        for(unsigned scale:{1,4,16}) {
            const auto packed=quantize_draw_frame(bytes,scale);
            assert(packed==imgui_quic_scalar_reference::quantize_draw_frame(bytes,scale));
            if(packed[0]==1)++fallback_count;else ++packed_count;
        }
    }
    assert(packed_count && fallback_count);
    std::printf("Quantization: half ties, bounds, NaN/Inf, origins and 300000 differential cases passed (%zu packed, %zu fallback)\n",packed_count,fallback_count);
}
