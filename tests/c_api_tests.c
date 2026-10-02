#include "imgui_quic.h"
#include "imgui_quic_imgui.h"
#include <assert.h>
#include <stdio.h>
#include <time.h>

int main(void) {
    imgui_quic_config_t config = {0};
    assert(imgui_quic_init(&config) == 0);
    for (int i = 0; i < 2; ++i) {
        imgui_quic_new_frame();
        igBegin("C API regression", NULL, 0);
        igTextUnformatted("Native C API generates draw data", NULL);
        ImDrawList* list = igGetWindowDrawList();
        const int vertices_before = list->VtxBuffer.Size;
        ImDrawList_AddTriangleFilled(list, (ImVec2){100,100}, (ImVec2){120,100},
                                    (ImVec2){110,120}, 0xff00ffffu);
        assert(list->VtxBuffer.Size > vertices_before);
        igEnd();
        imgui_quic_render();
    }
    ImDrawData* draw = igGetDrawData();
    assert(draw && draw->Valid && draw->TotalVtxCount > 0 && draw->TotalIdxCount > 0);
    const double before = igGetTime();
    const struct timespec delay = {0, 80000000};
    nanosleep(&delay, NULL);
    imgui_quic_new_frame();
    assert(igGetTime() - before >= 0.06);
    imgui_quic_render();
    imgui_quic_shutdown();
    puts("C ABI links and produces native draw data");
    return 0;
}
