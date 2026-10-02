#include "draw_protocol.hpp"
#include "imgui.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <random>
using namespace imgui_quic_core;

static std::vector<uint8_t> geometry(size_t n, uint32_t color=0xff112233) {
    auto bytes=draw_header(FrameHeader{0,0,1280,720,1,1});
    std::vector<ImDrawVert> vertices(n);
    std::vector<uint32_t> indices(n);
    for(size_t i=0;i<n;++i) { vertices[i]={{float(i%200),float(i/200)},{0,0},color}; indices[i]=uint32_t(i); }
    imgui_quic_draw_cmd_t cmd{{0,0,1280,720},1,0,0,uint32_t(n)};
    append_draw_list(bytes,vertices.data(),uint32_t(n),indices.data(),uint32_t(n),4,&cmd,1);
    return bytes;
}
static std::vector<uint8_t> queued(State& state,ClientId id) {
    std::vector<uint8_t> result;
    state.with_clients([&](auto& clients){auto& c=clients.at(id); std::lock_guard<std::mutex> lock(c.out->mtx);
        assert(c.out->frame); result=*c.out->frame;}); return result;
}
static uint32_t word(const std::vector<uint8_t>& b,size_t p) {
    return uint32_t(b[p])|(uint32_t(b[p+1])<<8)|(uint32_t(b[p+2])<<16)|(uint32_t(b[p+3])<<24);
}
static float get_float(const std::vector<uint8_t>& b,size_t off) {
    uint32_t bits=word(b,off); float f; std::memcpy(&f,&bits,4); return f;
}
static void set_float(std::vector<uint8_t>& b,size_t off,float f) {
    uint32_t bits; std::memcpy(&bits,&f,4);
    for(int j=0;j<4;++j)b[off+j]=uint8_t(bits>>(8*j));
}
static std::vector<uint8_t> moved(std::vector<uint8_t> b,float dx,float dy) {
    size_t off=29;
    for(uint32_t i=0;i<word(b,25);++i) {
        uint32_t nv=word(b,off),ni=word(b,off+4),nc=word(b,off+8);
        for(uint32_t j=0;j<nv;++j) {
            const size_t v=off+12+size_t(j)*20;
            set_float(b,v,get_float(b,v)+dx);set_float(b,v+4,get_float(b,v+4)+dy);
        }
        off+=12+size_t(nv)*20+size_t(ni)*4+size_t(nc)*36;
    }
    return b;
}
static void write_record(const std::vector<uint8_t>& bytes) {
    uint32_t n=uint32_t(bytes.size()); uint8_t h[4];
    for(int i=0;i<4;++i)h[i]=uint8_t(n>>(8*i));
    fwrite(h,1,4,stdout); fwrite(bytes.data(),1,bytes.size(),stdout);
}
int main(int argc,char**) {
    if(argc>1) {
        // Cross-language fixture: compare the JS reconstruction byte-for-byte
        // to original C++ geometry, including growth, shrinkage and empty UI.
        DrawSnapshot base{1,geometry(300)};
        write_record(encode_draw_frame(base,nullptr)); write_record(base.bytes);
        std::mt19937 rng(42);
        for(uint32_t i=2;i<202;++i) {
            auto bytes= i%5==0 ? geometry(rng()%600) : base.bytes;
            if(i%7==0) bytes=draw_header(FrameHeader{0,0,1280,720,1,1});
            // Change valid vertex colors without corrupting draw metadata.
            if(bytes.size()>100) for(int j=0;j<20;++j)bytes[41+16+(rng()%3)]=uint8_t(rng());
            DrawSnapshot frame{i,std::move(bytes)};
            write_record(encode_draw_frame(frame,&base)); write_record(frame.bytes);
            base=std::move(frame);
        }
        // Multiple lists, fractional/negative translations, stationary clips,
        // local edits and size changes must survive cross-language round trips.
        auto multiple=geometry(300),extra=geometry(50);
        multiple.insert(multiple.end(),extra.begin()+29,extra.end());multiple[25]=2;
        base=DrawSnapshot{202,multiple};
        write_record(encode_draw_frame(base,nullptr,true));write_record(base.bytes);
        for(uint32_t i=203;i<303;++i) {
            auto bytes=moved(base.bytes,i%2?0.1f:-5.25f,i%3?0.3f:-12.0f);
            if(i==249) bytes=moved(base.bytes,1.0e8f,-1.0e8f);
            if(i==250) bytes=moved(base.bytes,-1.0e8f,1.0e8f);
            bytes[57]^=uint8_t(i); // residual color edit
            set_float(bytes,61,get_float(bytes,61)+0.125f); // nonuniform local movement
            if(i%17==0)bytes=geometry(300+i%20); // topology change
            DrawSnapshot frame{i,std::move(bytes)};
            auto old=encode_draw_frame(frame,&base),wire=encode_draw_frame(frame,&base,true);
            assert(wire.size()<=old.size());
            write_record(wire);write_record(frame.bytes);base=std::move(frame);
        }
        return 0;
    }
    auto small=draw_header(FrameHeader{0,0,1280,720,1,1});
    ImDrawVert vertices[4]{}; uint16_t indices[3]={0,1,2};
    imgui_quic_draw_cmd_t command{{0,0,1280,720},1,1,0,3};
    append_draw_list(small,vertices,4,indices,3,2,&command,1);
    assert(word(small,41+80+8)==2); // 16-bit indices widened to u32
    assert(word(small,41+80+12+28)==1); // nonzero base vertex preserved
    DrawSnapshot a{1,geometry(300)}, b{2,a.bytes};
    assert(encode_draw_frame(a,nullptr)[0]==0x0d);
    auto same=encode_draw_frame(b,&a); assert(same[0]==0x0e && same.size()==13);
    b.bytes[60]^=1; auto delta=encode_draw_frame(b,&a);
    assert(delta[0]==0x0e && delta.size()<40);
    DrawSnapshot noise{3,std::vector<uint8_t>(a.bytes.size(),0x55)};
    assert(encode_draw_frame(noise,&a)[0]==0x0d);
    DrawSnapshot translated{4,moved(a.bytes,15.25f,-8.5f)};
    auto motion=encode_draw_frame(translated,&a,true);
    assert(motion[0]==0x0f && motion.size()==29);
    assert(encode_draw_frame(translated,&a)[0]!=0x0f); // capability fallback
    State modes;
    auto old_client=modes.add_client(),new_client=modes.add_client();
    modes.set_client_capabilities(old_client,kDrawIPCapability);
    modes.set_client_capabilities(new_client,kDrawIPCapability|kDrawMotionCapability);
    modes.publish_draw_frame(a.bytes);
    modes.acknowledge_draw(old_client,word(queued(modes,old_client),1));
    modes.acknowledge_draw(new_client,word(queued(modes,new_client),1));
    modes.publish_draw_frame(translated.bytes);
    assert(queued(modes,old_client)[0]==0x0e);
    assert(queued(modes,new_client)[0]==0x0f);
    State state; auto client=state.add_client(); state.set_client_capabilities(client,kDrawIPCapability);
    state.publish_draw_frame(a.bytes); auto first=queued(state,client); assert(first[0]==0x0d);
    state.acknowledge_draw(client,999999); // Cannot promote a frame never sent.
    state.publish_draw_frame(b.bytes); assert(queued(state,client)==first);
    state.with_clients([&](auto& c){
        assert(c.at(client).draw_pending.size()==1);
        c.at(client).draw_ack_time=std::chrono::steady_clock::now()-std::chrono::seconds(11);
    });
    state.publish_draw_frame(b.bytes);
    state.with_clients([&](auto& c){assert(!c.at(client).out->closed);});
    state.acknowledge_draw(client,word(first,1));
    for(int i=0;i<20;++i) { b.bytes[60]=uint8_t(i); state.publish_draw_frame(b.bytes); }
    auto last=queued(state,client); assert(last[0]==0x0e && word(last,5)==word(first,1));
    state.with_clients([&](auto& c){assert(c.at(client).draw_pending.size()==kDrawWindow);
        assert(c.at(client).draw_pending_sizes.size()==kDrawWindow);
        assert(c.at(client).draw_pending_bytes<=kDrawByteWindow);
        assert(c.at(client).out->control.empty());}); // coalescing needs no reset
    state.acknowledge_draw(client,word(last,1));
    state.with_clients([&](auto& c){assert(c.at(client).draw_pending_bytes==0 && c.at(client).draw_pending_sizes.empty());});
    state.publish_draw_frame(a.bytes); auto next=queued(state,client);
    assert(next[0]==0x0e && word(next,5)==word(last,1));
    state.acknowledge_draw(client,0); state.publish_draw_frame(a.bytes);
    assert(queued(state,client)[0]==0x0d);
    auto peer=state.add_client(); state.set_client_capabilities(peer,kDrawIPCapability);
    state.publish_draw_frame(b.bytes); assert(queued(state,peer)[0]==0x0d);
    std::vector<uint8_t> ack{0x1d,uint8_t(client),0,0,0,42,0,0,0};
    auto parsed=parse_client_msgs(ack.data(),ack.size());
    assert(parsed && parsed->at(0).second.kind==ClientMsg::Kind::DrawAck && parsed->at(0).second.draw_ack==42);
    ack.pop_back(); assert(!parse_client_msgs(ack.data(),ack.size()));
    // Pixel-only updates force another presentation even with identical geometry.
    State pixels; auto pc=pixels.add_client(); pixels.set_client_capabilities(pc,kDrawIPCapability);
    pixels.publish_draw_frame(a.bytes); auto image=queued(pixels,pc);
    pixels.acknowledge_draw(pc,word(image,1));
    pixels.publish_draw_frame(a.bytes); assert(queued(pixels,pc)==image);
    const uint8_t rgba[4]={255,0,0,255};
    pixels.send_texture(42,make_texture_msg(42,1,1,rgba,4));
    pixels.publish_draw_frame(a.bytes); assert(word(queued(pixels,pc),1)!=word(image,1));
    pixels.with_clients([&](auto& clients){
        auto& c=clients.at(pc);
        c.draw_ack_time=std::chrono::steady_clock::now()-std::chrono::seconds(11);
    });
    pixels.publish_draw_frame(b.bytes);
    pixels.with_clients([&](auto& clients){assert(clients.at(pc).out->closed);});
    // Bulk deltas cannot fill an eight-frame reliable queue. An oversized
    // frame still gets through alone, and ACK/recovery release its byte budget.
    State bulk; auto bc=bulk.add_client(); bulk.set_client_capabilities(bc,kDrawIPCapability);
    auto big=geometry(3000);
    bulk.publish_draw_frame(big); auto initial=queued(bulk,bc);
    bulk.acknowledge_draw(bc,word(initial,1));
    std::fill(big.begin()+40,big.end(),0x55);
    bulk.publish_draw_frame(big); auto oversized=queued(bulk,bc);
    assert(oversized.size()>kDrawByteWindow);
    big[50]=0x66; bulk.publish_draw_frame(big);
    assert(queued(bulk,bc)==oversized);
    bulk.acknowledge_draw(bc,999999);
    bulk.with_clients([&](auto& c){assert(c.at(bc).draw_pending_bytes==oversized.size());});
    bulk.acknowledge_draw(bc,word(oversized,1));
    bulk.with_clients([&](auto& c){assert(c.at(bc).draw_pending_bytes==0);});
    bulk.publish_draw_frame(big); assert(word(queued(bulk,bc),1)!=word(oversized,1));
    bulk.acknowledge_draw(bc,0);
    bulk.with_clients([&](auto& c){assert(c.at(bc).draw_pending_bytes==0 && c.at(bc).draw_pending_sizes.empty());});
    bulk.publish_draw_frame(big);
    bulk.with_clients([&](auto& c){c.at(bc).draw_ack_time=std::chrono::steady_clock::now()-std::chrono::seconds(61);});
    bulk.publish_draw_frame(big);
    bulk.with_clients([&](auto& c){assert(c.at(bc).out->closed);});
    // Event-driven transports must wake for resources, frames, recovery and
    // disconnect, and a producer retaining the mailbox must retain its notifier.
    State notified;
    unsigned wakes=0;
    auto lifetime=std::make_shared<int>(1);
    std::weak_ptr<int> weak=lifetime;
    auto nc=notified.add_client(0,[lifetime,&wakes]{++wakes;});
    lifetime.reset();
    notified.set_client_capabilities(nc,kDrawIPCapability);
    std::shared_ptr<OutBox> retained;
    notified.with_clients([&](auto& c){retained=c.at(nc).out;});
    notified.send_control_to(nc,{6}); assert(wakes==1);
    notified.publish_draw_frame(a.bytes); assert(wakes==2);
    notified.acknowledge_draw(nc,0); assert(wakes==3);
    notified.remove_client(nc); assert(wakes==4 && retained->closed);
    assert(!weak.expired());
    retained.reset(); assert(weak.expired());
    puts("draw protocol tests passed");
}
