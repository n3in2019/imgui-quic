#include "imgui_quic.h"
#include "imgui_quic_internal.h"

#include <cstdint>

#include "imgui.h"
#include <vector>
#include "imgui_internal.h"  // input trickle queue

static const imgui_quic_core_api_t* GImGuiQuicCore = nullptr;

struct ImGuiQuicBackendData {
    imgui_quic_backend_t* Core;
};

static char GClipboardBuf[4096];

static const char* ImGuiQuic_GetClipboardText(void* user_data) {
    (void)user_data;
    if (GImGuiQuicCore && GImGuiQuicCore->get_clipboard_text) {
        GImGuiQuicCore->get_clipboard_text(GClipboardBuf, sizeof(GClipboardBuf));
    } else {
        GClipboardBuf[0] = '\0';
    }
    return GClipboardBuf;
}

static void ImGuiQuic_SetClipboardText(void* user_data, const char* text) {
    (void)user_data;
    if (GImGuiQuicCore && GImGuiQuicCore->set_clipboard_text && text) {
        GImGuiQuicCore->set_clipboard_text(text);
    }
}

static ImGuiQuicBackendData* ImGuiQuic_GetBackendData() {
    return ImGui::GetCurrentContext() ? (ImGuiQuicBackendData*)ImGui::GetIO().BackendRendererUserData
                                      : nullptr;
}

extern "C" void imgui_quic_imgui_backend_set_core_api(const imgui_quic_core_api_t* api) {
    GImGuiQuicCore = api;
}

extern "C" void imgui_quic_imgui_backend_new_frame(imgui_quic_backend_t* backend) {
    ImGuiIO& io = ImGui::GetIO();

    // Preserve the viewport while ImGui trickles already-queued input.
    imgui_quic_frame_info_t frame_info;
    GImGuiQuicCore->backend_new_frame(backend, ImGui::GetTime(),
                                       GImGui->InputEventsQueue.Size != 0, &frame_info);
    io.DeltaTime = frame_info.delta_time;
    io.DisplaySize = ImVec2(frame_info.display_w, frame_info.display_h);
    // The active client's devicePixelRatio: reaches the browser in the
    // draw-data header (fbsx/fbsy). Layout itself stays in CSS pixels.
    io.DisplayFramebufferScale = ImVec2(frame_info.display_scale, frame_info.display_scale);

    imgui_quic_event_t ev;
    while (GImGuiQuicCore->backend_poll_event(backend, &ev)) {
        switch (ev.type) {
            case IMGUI_QUIC_EVENT_MOUSE_MOVE:
                io.AddMousePosEvent(ev.mouse_move.x, ev.mouse_move.y);
                break;
            case IMGUI_QUIC_EVENT_MOUSE_DOWN:
                io.AddMouseButtonEvent(ev.mouse_button.button, true);
                break;
            case IMGUI_QUIC_EVENT_MOUSE_UP:
                io.AddMouseButtonEvent(ev.mouse_button.button, false);
                break;
            case IMGUI_QUIC_EVENT_MOUSE_WHEEL:
                io.AddMouseWheelEvent(ev.mouse_wheel.dx, ev.mouse_wheel.dy);
                break;
            case IMGUI_QUIC_EVENT_KEY_DOWN:
                io.AddKeyEvent((ImGuiKey)ev.key.key, true);
                break;
            case IMGUI_QUIC_EVENT_KEY_UP:
                io.AddKeyEvent((ImGuiKey)ev.key.key, false);
                break;
            case IMGUI_QUIC_EVENT_TEXT_INPUT:
                io.AddInputCharacter(ev.text.ch);
                break;
        }
    }

    io.AddFocusEvent(true);
    ImGui::NewFrame();
}

static void ImGuiQuic_HandleTexture(imgui_quic_backend_t* backend, ImTextureData* tex) {
    if (backend == nullptr || tex == nullptr) return;

    if (tex->Status == ImTextureStatus_WantCreate) {
        uint64_t id = GImGuiQuicCore->backend_alloc_texture_id(backend);
        tex->SetTexID((ImTextureID)(uintptr_t)id);
        tex->SetStatus(ImTextureStatus_OK);

        if (tex->GetPixels()) {
            uint32_t byte_count = (uint32_t)tex->GetSizeInBytes();
            GImGuiQuicCore->send_texture(id, (const uint8_t*)tex->GetPixels(), byte_count,
                                     (uint32_t)tex->Width, (uint32_t)tex->Height);
        }
    } else if (tex->Status == ImTextureStatus_WantUpdates) {
        if (tex->GetPixels()) {
            uint32_t byte_count = (uint32_t)tex->GetSizeInBytes();
            GImGuiQuicCore->send_texture((uint64_t)(uintptr_t)tex->GetTexID(),
                                     (const uint8_t*)tex->GetPixels(), byte_count,
                                     (uint32_t)tex->Width, (uint32_t)tex->Height);
        }
        tex->SetStatus(ImTextureStatus_OK);
    } else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0) {
        tex->SetTexID(ImTextureID_Invalid);
        tex->SetStatus(ImTextureStatus_Destroyed);
    }
}

extern "C" void imgui_quic_imgui_backend_render_draw_data(imgui_quic_backend_t* backend,
                                                    const void* draw_data_ptr) {
    const ImDrawData* draw_data = (const ImDrawData*)draw_data_ptr;
    if (draw_data == nullptr) return;
    if (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f) return;

    if (draw_data->Textures != nullptr) {
        for (ImTextureData* tex : *draw_data->Textures) {
            if (tex->Status != ImTextureStatus_OK) {
                ImGuiQuic_HandleTexture(backend, tex);
            }
        }
    }

    const int frame_mode = GImGuiQuicCore->begin_frame(draw_data->DisplayPos.x, draw_data->DisplayPos.y,
                                 draw_data->DisplaySize.x, draw_data->DisplaySize.y,
                                 draw_data->FramebufferScale.x, draw_data->FramebufferScale.y);
    if (!frame_mode) {
        return;
    }

    for (const ImDrawList* list : draw_data->CmdLists) {
        std::vector<imgui_quic_draw_cmd_t> commands;
        commands.reserve(list->CmdBuffer.Size);
        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            // Native renderer callbacks are not portable over the wire.
            if (cmd.UserCallback) continue;
            commands.push_back({{cmd.ClipRect.x,cmd.ClipRect.y,cmd.ClipRect.z,cmd.ClipRect.w},
                                uint64_t(cmd.GetTexID()),cmd.VtxOffset,cmd.IdxOffset,cmd.ElemCount});
        }
        GImGuiQuicCore->add_draw_list(list->VtxBuffer.Data,list->VtxBuffer.Size,
            list->IdxBuffer.Data,list->IdxBuffer.Size,sizeof(ImDrawIdx),
            commands.data(),uint32_t(commands.size()));
    }
    GImGuiQuicCore->end_frame();
}

extern "C" bool imgui_quic_imgui_backend_init(void) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    ImGuiQuicBackendData* bd = IM_NEW(ImGuiQuicBackendData)();
    bd->Core = GImGuiQuicCore->backend_create();
    if (!bd->Core) {
        IM_DELETE(bd);
        ImGui::DestroyContext();
        return false;
    }

    io.BackendRendererUserData = (void*)bd;
    io.BackendPlatformUserData = (void*)bd;
    io.BackendPlatformName = "imgui_impl_imgui_quic";
    io.BackendRendererName = "imgui_impl_imgui_quic";
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

    io.GetClipboardTextFn = ImGuiQuic_GetClipboardText;
    io.SetClipboardTextFn = ImGuiQuic_SetClipboardText;

    io.Fonts->Build();

    return true;
}

extern "C" void imgui_quic_imgui_backend_shutdown() {
    ImGuiQuicBackendData* bd = ImGuiQuic_GetBackendData();
    if (bd) {
        ImGuiIO& io = ImGui::GetIO();
        io.BackendRendererUserData = nullptr;
        io.BackendPlatformUserData = nullptr;
        io.BackendPlatformName = nullptr;
        io.BackendRendererName = nullptr;
        GImGuiQuicCore->backend_destroy(bd->Core);
        IM_DELETE(bd);
    }
    if (ImGui::GetCurrentContext()) {
        ImGui::DestroyContext();
    }
}

extern "C" void imgui_quic_imgui_backend_begin_frame() {
    ImGuiQuicBackendData* bd = ImGuiQuic_GetBackendData();
    IM_ASSERT(bd != nullptr && "Did you call imgui_quic_init()?");
    imgui_quic_imgui_backend_new_frame(bd->Core);
}

extern "C" void imgui_quic_imgui_backend_render() {
    ImGuiQuicBackendData* bd = ImGuiQuic_GetBackendData();
    if (!bd) return;
    ImGui::Render();
    imgui_quic_imgui_backend_render_draw_data(bd->Core, ImGui::GetDrawData());
}
