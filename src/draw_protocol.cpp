#include "draw_protocol.hpp"
#include "imgui.h"
#include "lz4.h"
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
            if(i+32<=a.size() && i+32<=b.size() &&
               std::memcmp(a.data()+i,b.data()+i,32)==0) { i+=32; continue; }
            if(!differs(i)) { ++i; continue; }
            size_t start=i++, end=i;
            // Include short equal runs: a separate patch costs 8 bytes.
            while(i<a.size() && i-end<=8) {
                // Eight bytes starting at the last change cannot contain a
                // gap long enough to split a patch. Inspect them together,
                // then trim the equal suffix to retain the byte-exact ranges.
                if(i==end && i+8<=a.size() && i+8<=b.size()) {
                    uint64_t av,bv;
                    std::memcpy(&av,a.data()+i,8);
                    std::memcpy(&bv,b.data()+i,8);
                    if(av!=bv) {
                        size_t last=i+8;
                        while(a[last-1]==b[last-1]) --last;
                        end=last;
                    }
                    i+=8;
                    continue;
                }
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
    if(13+frame.bytes.size()>=bound) return {};
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
template<size_t Stride>
void planar_residual(const std::vector<uint8_t>& bytes, const DrawSnapshot* base,
                     std::vector<uint8_t>& residual) {
    size_t destination=0;
    const size_t common=base?std::min(bytes.size(),base->bytes.size()):0;
    // Separate the shared prefix from zero extension so the hot lane loop has
    // no per-byte baseline/null/bounds branches, including topology changes.
    for(size_t lane=0;lane<Stride;++lane) {
        size_t i=lane;
        if(base) {
            for(;i<common;i+=Stride)
                residual[destination++]=uint8_t(bytes[i]-base->bytes[i]);
        }
        for(;i<bytes.size();i+=Stride)
            residual[destination++]=bytes[i];
    }
}
bool sampled_attributes_changed(const std::vector<uint8_t>& a,const std::vector<uint8_t>& b) {
    if(a.size()!=b.size() || a.size()<29 || a[0]!=b[0] || read_u32(a,25)!=read_u32(b,25)) return true;
    const bool packed=a[0]==0x21 || a[0]==0x24 || a[0]==0x25;
    if(a[0]!=1 && !packed) return true;
    const size_t stride=packed?12:20, header=packed?20:12;
    size_t off=29;
    for(uint32_t list=0;list<read_u32(a,25);++list) {
        if(a.size()-off<header || std::memcmp(a.data()+off,b.data()+off,header)!=0) return true;
        const uint32_t nv=read_u32(a,off),ni=read_u32(a,off+4),nc=read_u32(a,off+8);
        const uint64_t length=header+uint64_t(nv)*stride+uint64_t(ni)*4+uint64_t(nc)*36;
        if(length>a.size()-off) return true;
        const size_t samples=std::min<uint32_t>(nv,64);
        for(size_t j=0;j<samples;++j) {
            const size_t vertex=samples<=1?0:j*(nv-1)/(samples-1);
            const size_t at=off+header+vertex*stride;
            if(std::memcmp(a.data()+at,b.data()+at,stride-4)!=0) return true;
        }
        off+=size_t(length);
    }
    return off!=a.size();
}
}
std::vector<uint8_t> quantize_draw_frame(const std::vector<uint8_t>& bytes, unsigned scale) {
    std::vector<VertexRange> ranges;
    if((scale!=16 && scale!=4 && scale!=1) || bytes.size()>kMaxDrawFrameBytes || !vertex_ranges(bytes,ranges)) return bytes;
    size_t length=bytes.size()+ranges.size()*8;
    for(const auto& range:ranges) length-=size_t(range.count)*8;
    if(length>=bytes.size()) return bytes;
    // Supported scales are powers of two: multiplying by their reciprocal
    // is exact, and avoids two runtime divisions for every vertex.
    const double inverse_scale=1.0/scale, max_error=0.5*inverse_scale;
    std::vector<uint8_t> packed(length);
    std::memcpy(packed.data(),bytes.data(),29);
    packed[0]=scale==16?0x21:scale==4?0x24:0x25;
    size_t out=29;
    auto u16=[&](uint16_t v) { packed[out++]=uint8_t(v); packed[out++]=uint8_t(v>>8); };
    for(const auto& range:ranges) {
        const size_t start=range.start;
        const uint32_t nv=range.count, ni=read_u32(bytes,start-8), nc=read_u32(bytes,start-4);
        std::memcpy(packed.data()+out,bytes.data()+start-12,12);out+=12;
        const float ox=nv?read_f32(bytes,start):0, oy=nv?read_f32(bytes,start+4):0;
        if(!std::isfinite(ox) || !std::isfinite(oy)) return bytes;
        if(nv) std::memcpy(packed.data()+out,bytes.data()+start,8);
        out+=8;
        for(uint32_t i=0;i<nv;++i) {
            size_t p=start+size_t(i)*20;
            for(int axis=0;axis<2;++axis) {
                const float v=read_f32(bytes,p+axis*4), origin=axis?oy:ox;
                const double relative=(double(v)-origin)*scale;
                // Range-check before integer conversion, including NaN/Inf.
                // Truncating after a half-unit bias implements ties away from
                // zero without a libm rounding call for each coordinate.
                if(!std::isfinite(relative) || relative<-32769 || relative>32768) return bytes;
                const int q=int(relative+(relative>=0?0.5:-0.5));
                if(q<-32768 || q>32767 ||
                   std::abs(double(float(double(origin)+double(q)*inverse_scale))-v)>max_error) return bytes;
                u16(uint16_t(int16_t(q)));
            }
            for(int axis=0;axis<2;++axis) {
                const float uv=read_f32(bytes,p+8+axis*4);
                if(!std::isfinite(uv) || uv<0 || uv>1) return bytes;
                u16(uint16_t(double(uv)*65535+0.5));
            }
            std::memcpy(packed.data()+out,bytes.data()+p+16,4);out+=4;
        }
        const size_t tail=start+size_t(nv)*20;
        const size_t tail_size=size_t(ni)*4+size_t(nc)*36;
        std::memcpy(packed.data()+out,bytes.data()+tail,tail_size);out+=tail_size;
    }
    return packed;
}
static std::vector<uint8_t> encode_exact(const DrawSnapshot& frame, const DrawSnapshot* base,
                                       bool allow_motion, size_t bound) {
    if(!allow_motion || !base) return encode_ranges(frame,base,bound);
    std::vector<VertexRange> current,previous;
    if(!vertex_ranges(frame.bytes,current) || !vertex_ranges(base->bytes,previous)) return encode_ranges(frame,base,bound);
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
    if(!count) return encode_ranges(frame,base,bound);
    // Residual patches restore every differing bit, including fractional
    // float rounding, stationary sub-elements, UVs, and viewport-clamped clips.
    auto residual=encode_ranges(frame,&predicted,bound);
    if(residual.empty()) return encode_ranges(frame,base,bound);
    const size_t motion_size=residual.size()+4+motions.size();
    if(residual[0]!=0x0e || motion_size>=13+frame.bytes.size() || motion_size>=bound) return encode_ranges(frame,base,bound);
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
std::vector<uint8_t> encode_draw_frame(const DrawSnapshot& frame, const DrawSnapshot* base,
                                      bool allow_motion, bool allow_lz4, bool allow_planar) {
    if(frame.bytes.size()>kMaxDrawFrameBytes) throw std::length_error("draw frame exceeds protocol limit");
    if(!allow_lz4) return encode_exact(frame,base,allow_motion,std::numeric_limits<size_t>::max());
    // Tiny patches/motion are cheaper than starting a compressor. Stop scanning
    // once this candidate cannot fit; the full candidate is evaluated only if
    // needed to beat compression. Never reference a speculative baseline.
    auto small=encode_exact(frame,base,allow_motion,513);
    if(!small.empty()) return small;
    auto residual=frame.bytes;
    if(base) {
        const size_t common=std::min(residual.size(),base->bytes.size());
        size_t i=0;
        for(;i+8<=common;i+=8) {
            uint64_t a,b;
            std::memcpy(&a,residual.data()+i,8); std::memcpy(&b,base->bytes.data()+i,8);
            a^=b; std::memcpy(residual.data()+i,&a,8);
        }
        for(;i<common;++i) residual[i]^=base->bytes[i];
    }
    std::vector<uint8_t> compressed(13+LZ4_compressBound(int(residual.size())));
    const int size=LZ4_compress_default(reinterpret_cast<const char*>(residual.data()),
        reinterpret_cast<char*>(compressed.data()+13),int(residual.size()),int(compressed.size()-13));
    if(size<=0 || size_t(size)>=frame.bytes.size())
        return encode_exact(frame,base,allow_motion,std::numeric_limits<size_t>::max());
    compressed.resize(13+size_t(size));
    compressed[0]=base?0x23:0x22;
    const uint32_t header[]={frame.id,base?base->id:0,uint32_t(frame.bytes.size())};
    for(size_t i=0;i<3;++i) for(int j=0;j<4;++j) compressed[1+i*4+j]=uint8_t(header[i]>>(j*8));
    auto exact=encode_exact(frame,base,allow_motion,compressed.size()+1);
    auto best=exact.empty()?std::move(compressed):std::move(exact);
    // Preserve tiny-frame latency. Broad changes can benefit from subtraction
    // and attribute-byte lanes; pay the second compression cost only for bulk.
    if(!allow_planar || best.size()<4096) return best;
    // Stable position/UV samples usually indicate color-only animation, where
    // XOR already works well. This bounded heuristic skips a second compressor;
    // a missed opportunity affects size only, never reconstructed geometry.
    if(base && !sampled_attributes_changed(frame.bytes,base->bytes)) return best;
    const size_t stride=frame.bytes[0]==1?20:12;
    // Fixed strides let the compiler specialize address calculations.
    if(stride==20) planar_residual<20>(frame.bytes,base,residual);
    else planar_residual<12>(frame.bytes,base,residual);
    // Use the current winner as a bounded compressor destination. A candidate
    // that cannot fit is discarded without growing another worst-case buffer.
    std::vector<uint8_t> planar(best.size());
    const int n=LZ4_compress_default(reinterpret_cast<const char*>(residual.data()),
        reinterpret_cast<char*>(planar.data()+14),int(residual.size()),int(planar.size()-14));
    if(n<=0 || 14+size_t(n)>=best.size()) return best;
    planar.resize(14+size_t(n));planar[0]=base?0x27:0x26;planar[13]=uint8_t(stride);
    for(size_t i=0;i<3;++i) for(int j=0;j<4;++j) planar[1+i*4+j]=uint8_t(header[i]>>(j*8));
    return planar;
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
