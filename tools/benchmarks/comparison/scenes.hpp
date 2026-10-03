#pragma once
#include "imgui.h"
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
namespace benchmark_scene {
inline uint32_t hash(uint32_t x) { x^=x>>16;x*=0x7feb352d;x^=x>>15;x*=0x846ca68b;return x^(x>>16); }
inline void draw(const std::string& scene,unsigned tick) {
    ImGui::SetNextWindowPos({float(scene=="moving"?130+int(100*std::sin(tick*.04)):30),30});
    ImGui::SetNextWindowSize({1000,650});
    ImGui::Begin("Transport benchmark",nullptr,ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoResize);
    ImGui::TextUnformatted("Identical Dear ImGui scene / 30 FPS");
    if(scene=="static" || scene=="moving" || scene=="dynamic") {
        for(unsigned row=0;row<30;row++)ImGui::Text("Row %02u: value %08u",row,scene=="dynamic"?tick*1234567u+row:row);
    } else if(scene=="tables") {
        if(ImGui::BeginTable("data",8,ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg)) {
            for(unsigned row=0;row<32;++row) {
                ImGui::TableNextRow();
                for(unsigned col=0;col<8;++col) {
                    ImGui::TableSetColumnIndex(col);
                    ImGui::Text("%010u",tick*1234567u+row*17+col);
                }
            }
            ImGui::EndTable();
        }
    } else {
        auto* list=ImGui::GetWindowDrawList();
        const auto origin=ImGui::GetCursorScreenPos();
        if(scene=="plots") {
            std::vector<ImVec2> points(4096);
            for(unsigned i=0;i<points.size();++i)
                points[i]={origin.x+950.f*i/(points.size()-1),origin.y+280+200*std::sin(i*.02f+tick*.03f)};
            list->AddPolyline(points.data(),points.size(),0xff44ee88,0,1.5f);
        } else {
            const unsigned n=scene=="dense"?6000:256+(tick%17)*64;
            for(unsigned i=0;i<n;++i) {
                const auto seed=hash(i+13);
                const float x=origin.x+seed%940,y=origin.y+(seed>>12)%530;
                const uint32_t color=0xff000000 | (hash(i+tick*1234567u)&0xffffff);
                list->AddTriangleFilled({x,y},{x+4,y+1},{x+1,y+4},color);
            }
        }
        ImGui::Dummy({950,540});
    }
    ImGui::End();
}
}
