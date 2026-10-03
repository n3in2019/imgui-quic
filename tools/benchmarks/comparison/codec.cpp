#include "imgui.h"
#include "draw_protocol.hpp"
#include "imgui-ws/imgui-draw-data-compressor.h"
#include "Private/NetImgui_Shared.h"
#include "Private/NetImgui_CmdPackets.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <climits>
#include <string>

#include "lz4.h"
#include "remote_codec.hpp"
#include "scenes.hpp"
#ifdef EXPERIMENT_MESHOPT
#include "meshoptimizer.h"
#endif

using namespace imgui_quic_core;
namespace ni = NetImgui::Internal;
using Clock = std::chrono::steady_clock;
DrawSnapshot snapshot(ImDrawData* data, int tick) {
    DrawSnapshot result;
    result.id=tick+1;
    result.bytes=draw_header({data->DisplayPos.x,data->DisplayPos.y,data->DisplaySize.x,data->DisplaySize.y,
                            data->FramebufferScale.x,data->FramebufferScale.y});
    for (int i=0;i<data->CmdListsCount;i++) {
        auto* list=data->CmdLists[i];
        std::vector<imgui_quic_draw_cmd_t> commands;
        for (const auto& c:list->CmdBuffer) {
            imgui_quic_draw_cmd_t cmd{};
            std::memcpy(cmd.clip_rect,&c.ClipRect,16);
            cmd.texture_id=c.GetTexID(); cmd.vtx_offset=c.VtxOffset; cmd.idx_offset=c.IdxOffset; cmd.elem_count=c.ElemCount;
            commands.push_back(cmd);
        }
        append_draw_list(result.bytes,list->VtxBuffer.Data,list->VtxBuffer.Size,list->IdxBuffer.Data,
                         list->IdxBuffer.Size,sizeof(ImDrawIdx),commands.data(),commands.size());
    }
    return result;
}
int main(int argc,char** argv) {
    if(argc!=4 && argc!=5)return 2;
    const bool verify=argc==5 && std::string(argv[4])=="verify";
    const bool fixtures=argc==5 && std::string(argv[4])=="fixtures";
    std::string algorithm=argv[1],scene=argv[2]; int frames=std::stoi(argv[3]);
    if(fixtures && (algorithm.find("plane")!=std::string::npos || algorithm.find("meshopt")!=std::string::npos)) {
        std::fprintf(stderr,"Research candidates have no negotiated wire fixtures; use verify.\n");
        return 2;
    }
    ImGui::CreateContext(); auto& io=ImGui::GetIO();
    io.IniFilename=nullptr;io.DisplaySize={1280,720};io.DeltaTime=1.f/30;
    unsigned char* pixels;int w,h;io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);io.Fonts->SetTexID(1);
    ImDrawDataCompressor::XorRlePerDrawListWithVtxOffset ws;
    remote_codec::WebSocketServer remote;
    std::vector<std::vector<char>> wsPrevious;
    DrawSnapshot previous;
    ni::CmdDrawFrame* netPrevious=nullptr;
    std::vector<double> times,sizes;double first=0;int vertices=0,indices=0,minVertices=INT_MAX,maxVertices=0;
    for(int tick=0;tick<frames+30;tick++) {
        ImGui::NewFrame();
        benchmark_scene::draw(scene,tick);
        ImGui::Render();auto* data=ImGui::GetDrawData();
        // Initial ImGui layout may be hidden; exclude empty startup from baselines.
        if(!data->CmdListsCount){--tick;continue;}
        auto start=Clock::now();size_t bytes=0;
        std::vector<uint8_t> fixtureWire;
        if(algorithm.rfind("imgui-quic",0)==0||algorithm=="raw") {
            auto frame=snapshot(data,tick);
            if(algorithm.find("q16")!=std::string::npos || algorithm.find("q4")!=std::string::npos || algorithm.find("q1")!=std::string::npos)
                frame.bytes=quantize_draw_frame(frame.bytes,algorithm.find("q16")!=std::string::npos?16:algorithm.find("q4")!=std::string::npos?4:1);
            if(algorithm=="raw")bytes=frame.bytes.size();
            else {
                std::vector<uint8_t> wire;
                if(algorithm.find("meshopt")!=std::string::npos) {
#ifdef EXPERIMENT_MESHOPT
                    const size_t stride=algorithm.find("20")!=std::string::npos?20:4;
                    auto residual=frame.bytes;
                    for(size_t i=0;i<std::min(residual.size(),previous.bytes.size());++i)residual[i]^=previous.bytes[i];
                    residual.resize((residual.size()+stride-1)/stride*stride);
                    wire.resize(13+meshopt_encodeVertexBufferBound(residual.size()/stride,stride));
                    const size_t size=meshopt_encodeVertexBuffer(wire.data()+13,wire.size()-13,residual.data(),residual.size()/stride,stride);
                    if(!size)return 12;
                    wire.resize(13+size);
                    if(verify) {
                        std::vector<uint8_t> decoded(residual.size());
                        if(meshopt_decodeVertexBuffer(decoded.data(),decoded.size()/stride,stride,wire.data()+13,size)!=0 || decoded!=residual)return 13;
                        decoded.resize(frame.bytes.size());
                        for(size_t i=0;i<std::min(decoded.size(),previous.bytes.size());++i)decoded[i]^=previous.bytes[i];
                        if(decoded!=frame.bytes)return 14;
                    }
#else
                    return 15;
#endif
                } else if(algorithm.find("plane")!=std::string::npos) {
                    const size_t stride=algorithm.find("plane20")!=std::string::npos?20:4;
                    std::vector<uint8_t> residual=frame.bytes;
                    const bool subtract=algorithm.find("sub")!=std::string::npos;
                    for(size_t i=0;i<std::min(residual.size(),previous.bytes.size());++i)
                        residual[i]=subtract?uint8_t(residual[i]-previous.bytes[i]):residual[i]^previous.bytes[i];
                    std::vector<uint8_t> planar(residual.size());
                    size_t destination=0;
                    for(size_t lane=0;lane<stride;++lane)
                        for(size_t i=lane;i<residual.size();i+=stride)planar[destination++]=residual[i];
                    wire.resize(13+LZ4_compressBound(int(planar.size())));
                    const int n=LZ4_compress_default(reinterpret_cast<const char*>(planar.data()),reinterpret_cast<char*>(wire.data()+13),planar.size(),wire.size()-13);
                    if(n<=0)return 9;
                    wire.resize(13+n);
                    if(verify) {
                        std::vector<uint8_t> restored(planar.size());
                        if(LZ4_decompress_safe(reinterpret_cast<const char*>(wire.data()+13),reinterpret_cast<char*>(restored.data()),n,restored.size())!=int(planar.size()) || restored!=planar)return 10;
                        std::vector<uint8_t> decoded(restored.size());
                        size_t source=0;
                        for(size_t lane=0;lane<stride;++lane)
                            for(size_t i=lane;i<decoded.size();i+=stride)decoded[i]=restored[source++];
                        for(size_t i=0;i<std::min(decoded.size(),previous.bytes.size());++i)
                            decoded[i]=subtract?uint8_t(decoded[i]+previous.bytes[i]):decoded[i]^previous.bytes[i];
                        if(decoded!=frame.bytes)return 11;
                    }
                } else if(algorithm.find("xor")!=std::string::npos) {
                    wire=frame.bytes;
                    const size_t common=std::min(wire.size(),previous.bytes.size());
                    size_t i=0;
                    for(;i+8<=common;i+=8) {
                        uint64_t a,b;std::memcpy(&a,wire.data()+i,8);std::memcpy(&b,previous.bytes.data()+i,8);
                        a^=b;std::memcpy(wire.data()+i,&a,8);
                    }
                    for(;i<common;i++)wire[i]^=previous.bytes[i];
                } else wire=encode_draw_frame(frame,previous.bytes.empty()?nullptr:&previous,true,algorithm.find("adaptive")!=std::string::npos,algorithm.find("live")!=std::string::npos);
                bytes=wire.size();
                if(fixtures)fixtureWire=wire;
                if(algorithm.find("lz4")!=std::string::npos) {
                    std::vector<char> compressed(LZ4_compressBound(wire.size()));
                    const int size=LZ4_compress(reinterpret_cast<const char*>(wire.data()),compressed.data(),wire.size());
                    if(size>0 && verify) {
                        std::vector<char> restored(wire.size());
                        if(LZ4_decompress_safe(compressed.data(),restored.data(),size,restored.size())!=int(wire.size()) ||
                           std::memcmp(restored.data(),wire.data(),wire.size()))return 7;
                    }
                    if(size>0)bytes=std::min(bytes,size_t(size)+4); // hypothetical decoded-length envelope
                }
                if(verify && algorithm.find("xor")!=std::string::npos) {
                    auto restored=wire;
                    for(size_t i=0;i<std::min(restored.size(),previous.bytes.size());++i)
                        restored[i]^=previous.bytes[i];
                    if(restored!=frame.bytes)return 8;
                }
            }
            if(algorithm.find("xor")!=std::string::npos)bytes+=13; // estimated frame/base/decoded-size envelope
            previous=std::move(frame);
        } else if(algorithm=="imgui-ws") {
            ws.setDrawData(data);bytes=ws.diffSize();
            if(verify) {
                const auto& full=ws.getDrawLists(); const auto& deltas=ws.getDrawListsDiff();
                for(size_t i=0;i<full.size();i++) {
                    uint32_t type;std::memcpy(&type,deltas[i].data(),4);
                    std::vector<char> decoded;
                    if(type==0)decoded.assign(deltas[i].begin()+4,deltas[i].end());
                    else {
                        decoded=wsPrevious.at(i);size_t out=0;
                        for(size_t j=4;j<deltas[i].size();j+=8) {
                            uint32_t count,value;std::memcpy(&count,deltas[i].data()+j,4);std::memcpy(&value,deltas[i].data()+j+4,4);
                            for(uint32_t k=0;k<count;k++) {
                                uint32_t word;std::memcpy(&word,decoded.data()+out,4);word^=value;
                                std::memcpy(decoded.data()+out,&word,4);out+=4;
                            }
                        }
                        if(out!=decoded.size())return 3;
                    }
                    if(decoded!=full[i])return 4;
                }
                wsPrevious=full;
            }
        } else if(algorithm=="RemoteImGui") {
            bytes=remote_codec::encode(remote,data);
        } else if(algorithm=="netImgui") {
            auto* current=ni::ConvertToCmdDrawFrame(data,ImGuiMouseCursor_Arrow);
            if(netPrevious) {
                auto* packed=ni::CompressCmdDrawFrame(netPrevious,current);
                bytes=packed->mSize;
                if(verify) {
                    auto* decoded=ni::DecompressCmdDrawFrame(netPrevious,packed);
                    if(decoded->mDrawGroupCount!=current->mDrawGroupCount)return 5;
                    for(unsigned i=0;i<current->mDrawGroupCount;i++) {
                        const auto& a=current->mpDrawGroups[i];const auto& b=decoded->mpDrawGroups[i];
                        if(std::memcmp(a.mpVertices.Get(),b.mpVertices.Get(),a.mVerticeCount*sizeof(ni::ImguiVert)) ||
                           std::memcmp(a.mpIndices.Get(),b.mpIndices.Get(),a.mIndiceCount*a.mBytePerIndex) ||
                           std::memcmp(a.mpDraws.Get(),b.mpDraws.Get(),a.mDrawCount*sizeof(ni::ImguiDraw)))return 6;
                    }
                    ni::netImguiDelete(decoded);
                }
                ni::netImguiDelete(packed);
            } else bytes=current->mSize;
            ni::netImguiDeleteSafe(netPrevious);netPrevious=current;
        } else return 2;
        double us=std::chrono::duration<double,std::micro>(Clock::now()-start).count();
        if(fixtures) {
            auto original=snapshot(data,tick);
            for(const auto* record:{&fixtureWire,&original.bytes}) {
                uint32_t n=record->size();unsigned char header[4];
                for(int i=0;i<4;i++)header[i]=uint8_t(n>>(8*i));
                fwrite(header,1,4,stdout);fwrite(record->data(),1,n,stdout);
            }
        }
        if(tick==0)first=bytes;
        if(tick>=30){times.push_back(us);sizes.push_back(bytes);minVertices=std::min(minVertices,data->TotalVtxCount);maxVertices=std::max(maxVertices,data->TotalVtxCount);}
        vertices=data->TotalVtxCount;indices=data->TotalIdxCount;
    }
    ni::netImguiDeleteSafe(netPrevious);
    auto mean=[](const auto& v){double sum=0;for(auto x:v)sum+=x;return sum/v.size();};
    double avg=mean(times),avgbytes=mean(sizes);std::sort(times.begin(),times.end());std::sort(sizes.begin(),sizes.end());
    if(!fixtures)std::printf("{\"algorithm\":\"%s\",\"scene\":\"%s\",\"frames\":%d,\"vertices\":%d,\"indices\":%d,\"first_bytes\":%.0f,\"mean_bytes\":%.3f,\"encode_mean_us\":%.3f,\"encode_p95_us\":%.3f,\"payload_p95_bytes\":%.0f,\"max_bytes\":%.0f,\"vertices_min\":%d,\"vertices_max\":%d}\n",algorithm.c_str(),scene.c_str(),frames,vertices,indices,first,avgbytes,avg,times[size_t(times.size()*.95)],sizes[size_t(sizes.size()*.95)],sizes.back(),minVertices,maxVertices);
    ImGui::DestroyContext();
}
