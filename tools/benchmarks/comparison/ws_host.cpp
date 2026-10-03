#include "imgui.h"
#include "imgui-ws/imgui-ws.h"
#include "scenes.hpp"
#include <chrono>
#include <csignal>
#include <cstdio>
#include <thread>
using Clock=std::chrono::steady_clock;
volatile std::sig_atomic_t running=1;
void stop(int){running=0;}
int main(int argc,char** argv) {
    if(argc!=2)return 2;
    ImGui::CreateContext();auto& io=ImGui::GetIO();
    io.IniFilename=nullptr;io.DisplaySize={1280,720};io.DeltaTime=1.f/30;
    unsigned char* pixels;int w,h;io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);io.Fonts->SetTexID(1);
    ImGuiWS server;
    if(!server.init(19444,".",{}))return 1;
    server.setTexture(1,ImGuiWS::Texture::Type::RGBA32,w,h,reinterpret_cast<char*>(pixels));
    std::signal(SIGTERM,stop);unsigned tick=0;auto next=Clock::now();
    std::fprintf(stderr,"Benchmark WebSocket listening\n");
    while(running) {
        for(const auto& e:server.takeEvents())if(e.type==ImGuiWS::Event::MouseMove)io.AddMousePosEvent(e.mouse_x,e.mouse_y);
        ImGui::NewFrame();
        auto* marker=ImGui::GetBackgroundDrawList();int start=marker->VtxBuffer.Size;
        marker->AddTriangleFilled({1,1},{3,1},{1,3},0xffffffff);
        const auto stamp=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
        marker->VtxBuffer[start].col=uint32_t(stamp);marker->VtxBuffer[start+1].col=uint32_t(uint64_t(stamp)>>32);
        marker->VtxBuffer[start+2].col=uint32_t(std::fmax(0.f,io.MousePos.x));
        benchmark_scene::draw(argv[1],tick++);ImGui::Render();server.setDrawData(ImGui::GetDrawData());
        next+=std::chrono::nanoseconds(1000000000/30);std::this_thread::sleep_until(next);
    }
    return 0;
}
