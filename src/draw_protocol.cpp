#include "draw_protocol.hpp"
#include "imgui.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include <cstddef>
#include <limits>
#include <type_traits>

namespace imgui_quic_core {
namespace {
bool native_little_endian() {
    const uint32_t one=1;
    return *reinterpret_cast<const uint8_t*>(&one)==1;
}
void u32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i=0;i<4;++i) out.push_back(uint8_t(v>>(8*i)));
}
void f32(std::vector<uint8_t>& out, float v) {
    uint32_t bits; std::memcpy(&bits,&v,4); u32(out,bits);
}
}
std::vector<uint8_t> draw_header(FrameHeader h) {
    std::vector<uint8_t> out{0x01};
    for (float f : {h.dpx,h.dpy,h.dsw,h.dsh,h.fbsx,h.fbsy}) f32(out,f);
    u32(out,0); return out;
}
void append_draw_list(std::vector<uint8_t>& out, const void* vertices, uint32_t nv,
                      const void* indices, uint32_t ni, int index_size,
                      const imgui_quic_draw_cmd_t* commands, uint32_t nc) {
    const uint64_t added=12+uint64_t(nv)*20+uint64_t(ni)*4+uint64_t(nc)*36;
    if (out.size()+added>kMaxDrawFrameBytes || (index_size!=2 && index_size!=4))
        throw std::length_error("draw frame exceeds protocol limit");
    out.reserve(out.size()+size_t(added));
    u32(out,nv); u32(out,ni); u32(out,nc);
    const auto* v=static_cast<const ImDrawVert*>(vertices);
    // Canonical Dear ImGui vertices already have the wire layout on little-endian
    // x86-64/AArch64. Keep the scalar path for custom layouts and other byte orders.
    constexpr bool packed_vertex = sizeof(ImDrawVert)==20 &&
        offsetof(ImDrawVert,pos)==0 && offsetof(ImDrawVert,uv)==8 &&
        offsetof(ImDrawVert,col)==16 && std::is_trivially_copyable<ImDrawVert>::value;
    if (packed_vertex && native_little_endian()) {
        const auto* bytes=static_cast<const uint8_t*>(vertices);
        if(nv) out.insert(out.end(),bytes,bytes+size_t(nv)*20);
    } else {
        for(uint32_t i=0;i<nv;++i) {
            f32(out,v[i].pos.x); f32(out,v[i].pos.y);
            f32(out,v[i].uv.x); f32(out,v[i].uv.y); u32(out,v[i].col);
        }
    }
    if(native_little_endian()) {
        const auto* bytes=static_cast<const uint8_t*>(indices);
        if(index_size==4) {
            if(ni) out.insert(out.end(),bytes,bytes+size_t(ni)*4);
        } else {
            const size_t offset=out.size(); out.resize(offset+size_t(ni)*4);
            auto* destination=out.data()+offset;
            for(uint32_t i=0;i<ni;++i) {
                uint16_t value; std::memcpy(&value,bytes+size_t(i)*2,2);
                const uint32_t widened=value;
                std::memcpy(destination+size_t(i)*4,&widened,4);
            }
        }
    } else {
        for(uint32_t i=0;i<ni;++i) {
            uint32_t n=0;
            if(index_size==2) { uint16_t x; std::memcpy(&x,static_cast<const uint8_t*>(indices)+i*2,2); n=x; }
            else std::memcpy(&n,static_cast<const uint8_t*>(indices)+i*4,4);
            u32(out,n);
        }
    }
    for(uint32_t i=0;i<nc;++i) {
        const auto& c=commands[i];
        for(float x:c.clip_rect) f32(out,x);
        u32(out,uint32_t(c.texture_id)); u32(out,uint32_t(c.texture_id>>32));
        u32(out,c.idx_offset); u32(out,c.vtx_offset); u32(out,c.elem_count);
    }
    uint32_t count=uint32_t(out[25])|(uint32_t(out[26])<<8)|(uint32_t(out[27])<<16)|(uint32_t(out[28])<<24);
    ++count;
    for(int i=0;i<4;++i) out[25+i]=uint8_t(count>>(8*i));
}
// With a bound no larger than the I frame, an empty result proves that this
// candidate cannot beat the already-constructed alternative. Patches only grow.
static std::vector<uint8_t> encode_ranges(const DrawSnapshot& frame, const DrawSnapshot* base,
    size_t bound=std::numeric_limits<size_t>::max()) {
    // 0x0d I / 0x0e P, frame id, base id (0 for I), decoded byte length.
    std::vector<uint8_t> out{0x0d};
    u32(out,frame.id); u32(out,0); u32(out,uint32_t(frame.bytes.size()));
    if(base) {
        const auto& a=frame.bytes; const auto& b=base->bytes;
        size_t i=0;
        auto differs=[&](size_t p){ return a[p] != (p<b.size()?b[p]:0); };
        while(i<a.size()) {
            if(!differs(i)) { ++i; continue; }
            size_t start=i++, end=i;
            // Include short equal runs: a separate patch costs 8 bytes.
            while(i<a.size() && i-end<=8) {
                if(differs(i)) end=i+1;
                ++i;
            }
            if(out.size()+8+(end-start)>=bound) return {};
            u32(out,uint32_t(start)); u32(out,uint32_t(end-start));
            out.insert(out.end(),a.begin()+start,a.begin()+end);
            if(out.size()>=13+a.size()) break;
        }
        if(out.size()<13+a.size()) {
            out[0]=0x0e;
            for(int j=0;j<4;++j) out[5+j]=uint8_t(base->id>>(8*j));
            return out;
        }
        out.resize(13);
    }
    out.insert(out.end(),frame.bytes.begin(),frame.bytes.end());
    return out;
}

namespace {
uint32_t read_u32(const std::vector<uint8_t>& b, size_t off) {
    return uint32_t(b[off]) | (uint32_t(b[off+1])<<8) |
           (uint32_t(b[off+2])<<16) | (uint32_t(b[off+3])<<24);
}
float read_f32(const std::vector<uint8_t>& b, size_t off) {
    uint32_t bits=read_u32(b,off); float value; std::memcpy(&value,&bits,4); return value;
}
void write_f32(std::vector<uint8_t>& b, size_t off, float value) {
    uint32_t bits; std::memcpy(&bits,&value,4);
    for(int i=0;i<4;++i) b[off+i]=uint8_t(bits>>(8*i));
}
struct VertexRange { size_t start; uint32_t count; };
bool vertex_ranges(const std::vector<uint8_t>& b, std::vector<VertexRange>& ranges) {
    if(b.size()<29 || b[0]!=1) return false;
    const uint32_t count=read_u32(b,25);
    size_t off=29;
    for(uint32_t i=0;i<count;++i) {
        if(b.size()-off<12) return false;
        uint32_t nv=read_u32(b,off),ni=read_u32(b,off+4),nc=read_u32(b,off+8);
        uint64_t size=12+uint64_t(nv)*20+uint64_t(ni)*4+uint64_t(nc)*36;
        if(size>b.size()-off) return false;
        ranges.push_back({off+12,nv}); off+=size_t(size);
    }
    return off==b.size();
}
}
std::vector<uint8_t> encode_draw_frame(const DrawSnapshot& frame, const DrawSnapshot* base,
                                       bool allow_motion) {
    if(!allow_motion || !base) return encode_ranges(frame,base);
    std::vector<VertexRange> current,previous;
    if(!vertex_ranges(frame.bytes,current) || !vertex_ranges(base->bytes,previous)) return encode_ranges(frame,base);
    DrawSnapshot predicted;
    std::vector<uint8_t> motions;
    uint32_t count=0;
    for(size_t i=0;i<std::min(current.size(),previous.size());++i) {
        const auto a=current[i],b=previous[i];
        // Reordered or resized lists fall back to byte patches. A prediction
        // never changes the canonical snapshot kept as the acknowledged base.
        if(!a.count || a.start!=b.start || a.count!=b.count) continue;
        const float dx=read_f32(frame.bytes,a.start)-read_f32(base->bytes,b.start);
        const float dy=read_f32(frame.bytes,a.start+4)-read_f32(base->bytes,b.start+4);
        if(!std::isfinite(dx) || !std::isfinite(dy) || (dx==0 && dy==0)) continue;
        if(predicted.bytes.empty()) { predicted.id=base->id; predicted.bytes=base->bytes; }
        u32(motions,uint32_t(i)); f32(motions,dx); f32(motions,dy); ++count;
        for(uint32_t j=0;j<b.count;++j) {
            const size_t off=b.start+size_t(j)*20;
            write_f32(predicted.bytes,off,read_f32(base->bytes,off)+dx);
            write_f32(predicted.bytes,off+4,read_f32(base->bytes,off+4)+dy);
        }
    }
    if(!count) return encode_ranges(frame,base);
    // Residual patches restore every differing bit, including fractional
    // float rounding, stationary sub-elements, UVs, and viewport-clamped clips.
    auto residual=encode_ranges(frame,&predicted);
    const size_t motion_size=residual.size()+4+motions.size();
    if(residual[0]!=0x0e || motion_size>=13+frame.bytes.size()) return encode_ranges(frame,base);
    // Evaluate motion first. Most window moves produce a tiny residual, so the
    // ordinary delta loses after only a few patches rather than a whole-frame scan.
    // +1 retains the original tie rule: motion must be strictly smaller.
    auto ordinary=encode_ranges(frame,base,motion_size+1);
    if(!ordinary.empty()) return ordinary;
    std::vector<uint8_t> out(residual.begin(),residual.begin()+13);
    out[0]=0x0f;
    u32(out,count); out.insert(out.end(),motions.begin(),motions.end());
    out.insert(out.end(),residual.begin()+13,residual.end());
    return out;
}
}

namespace imgui_quic_core {
std::vector<uint8_t> make_texture_msg(uint64_t id, uint32_t width, uint32_t height,
                                      const uint8_t* pixels, size_t len) {
    std::vector<uint8_t> out;
    out.reserve(1 + 8 + 4 + 4 + 4 + len);
    out.push_back(0x02);
    u32(out, uint32_t(id));
    u32(out, uint32_t(id >> 32));
    u32(out, width);
    u32(out, height);
    u32(out, uint32_t(len));
    out.insert(out.end(), pixels, pixels + len);
    return out;
}

}
