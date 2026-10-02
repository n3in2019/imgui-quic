// lib.cpp — public lifecycle C ABI + the core api table consumed by the
// shared ImGui backend (src/imgui_backend.cpp).

#include "core.hpp"
#include "draw_protocol.hpp"
#include "quic.hpp"

#include <string.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <stdexcept>

#include "imgui_quic.h"
#include "imgui_quic_texture.h"
#include "imgui_quic_internal.h"

namespace imgui_quic_core {
namespace {

struct GlobalCtx {
    std::shared_ptr<State> state;
    std::shared_ptr<QuicHandle> quic;
    uint64_t next_app_texture_id = UINT64_C(2147483648);
};

std::mutex g_mutex;
std::unique_ptr<GlobalCtx> g_global;
std::atomic<bool> g_initialized{false};
std::atomic<bool> g_backend_initialized{false};
// Guarded by g_mutex: set for the duration of an in-flight init so a racing
// second init cannot start a duplicate server. g_mutex is NOT held across the
// backend init below (backend callbacks call global(), which locks it).
bool g_initializing = false;

GlobalCtx* global() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_global ? g_global.get() : nullptr;
}

// --- backend state ------------------------------------------------------------

struct Backend {
    std::chrono::steady_clock::time_point time{};
    uint64_t next_texture_id = 1;
    float display_w = 1280.0f;
    float display_h = 720.0f;
    float display_scale = 1.0f;
};

// --- core api table implementation ---------------------------------------------

imgui_quic_backend_t* backend_create() {
    float size[2] = {1280.0f, 720.0f};
    if (GlobalCtx* g = global()) g->state->get_display_size(size);
    auto* b = new Backend();
    b->display_w = size[0];
    b->display_h = size[1];
    return reinterpret_cast<imgui_quic_backend_t*>(b);
}

void backend_destroy(imgui_quic_backend_t* backend) { delete reinterpret_cast<Backend*>(backend); }

void backend_new_frame(imgui_quic_backend_t* backend_ptr, double current_time, int keep_input_layout,
                       imgui_quic_frame_info_t* out_info) {
    if (backend_ptr == nullptr || out_info == nullptr) return;
    Backend* backend = reinterpret_cast<Backend*>(backend_ptr);

    (void)current_time;
    const auto now = std::chrono::steady_clock::now();
    float delta_time = backend->time.time_since_epoch().count() != 0
        ? std::chrono::duration<float>(now - backend->time).count() : 1.0f / 60.0f;
    backend->time = now;
    if (delta_time <= 0.0f) delta_time = 1.0f / 10000.0f;

    float size[2] = {backend->display_w, backend->display_h};
    float scale = backend->display_scale;
    if (GlobalCtx* g = global()) g->state->prepare_input_frame(size, scale, keep_input_layout != 0);
    backend->display_w = size[0];
    backend->display_h = size[1];
    backend->display_scale = scale;

    out_info->delta_time = delta_time;
    out_info->display_w = backend->display_w;
    out_info->display_h = backend->display_h;
    out_info->display_scale = backend->display_scale;
}

uint64_t backend_alloc_texture_id(imgui_quic_backend_t* backend_ptr) {
    if (backend_ptr == nullptr) return 0;
    Backend* backend = reinterpret_cast<Backend*>(backend_ptr);
    return backend->next_texture_id++;
}

thread_local std::vector<uint8_t> draw_bytes;

int begin_frame(float dpx, float dpy, float dsw, float dsh, float fbsx, float fbsy) {
    GlobalCtx* g = global();
    if (g == nullptr) return 0;
    draw_bytes.clear();
    if (g->state->wants_draw_frames())
        draw_bytes = draw_header(FrameHeader{dpx,dpy,dsw,dsh,fbsx,fbsy});
    if (!g->state->begin_frame(dpx, dpy, dsw, dsh, fbsx, fbsy)) return 0;
    return !draw_bytes.empty();
}

void add_draw_list(const void* v, uint32_t nv, const void* idx, uint32_t ni, int size,
                   const imgui_quic_draw_cmd_t* cmds, uint32_t nc) {
    if(draw_bytes.empty()) return;
    try { append_draw_list(draw_bytes,v,nv,idx,ni,size,cmds,nc); }
    catch(const std::length_error&) { draw_bytes.clear(); }
}

void end_frame() {
    GlobalCtx* g = global();
    if (g == nullptr) return;
    if(!draw_bytes.empty()) g->state->publish_draw_frame(std::move(draw_bytes));
}

void send_texture(uint64_t id, const uint8_t* pixels, uint32_t len, uint32_t width,
                  uint32_t height) {
    if (pixels == nullptr || len == 0) return;
    GlobalCtx* g = global();
    if (g == nullptr) return;
    std::vector<uint8_t> msg = make_texture_msg(id, width, height, pixels, len);
    g->state->send_texture(id, msg);
}

int backend_poll_event(imgui_quic_backend_t* backend_ptr, imgui_quic_event_t* out) {
    GlobalCtx* g = global();
    if (g == nullptr || out == nullptr) return 0;

    auto ev = g->state->try_poll_input();
    if (!ev.has_value()) return 0;

    memset(out, 0, sizeof(*out));
    out->type = ev->ev_type;
    out->display_w = ev->display_w;
    out->display_h = ev->display_h;
    switch (ev->ev_type) {
        case 0:
            out->mouse_move.x = ev->x;
            out->mouse_move.y = ev->y;
            break;
        case 1:
        case 2:
            out->mouse_button.button = ev->button;
            break;
        case 3:
            out->mouse_wheel.dx = ev->wheel_x;
            out->mouse_wheel.dy = ev->wheel_y;
            break;
        case 4:
        case 5:
            out->key.key = ev->key;
            break;
        case 6:
            out->text.ch = ev->character;
            break;
        default:
            break;
    }
    if (backend_ptr != nullptr && out->display_w > 0.0f && out->display_h > 0.0f) {
        Backend* backend = reinterpret_cast<Backend*>(backend_ptr);
        backend->display_w = out->display_w;
        backend->display_h = out->display_h;
    }
    return 1;
}

int get_clipboard_text(char* buf, int buf_size) {
    GlobalCtx* g = global();
    if (g == nullptr) {
        if (buf != nullptr && buf_size > 0) buf[0] = '\0';
        return 0;
    }
    std::string text = g->state->get_clipboard_text();
    int copy_len = int(std::min(text.size(), size_t(buf_size > 0 ? buf_size - 1 : 0)));
    if (copy_len > 0 && buf != nullptr) memcpy(buf, text.data(), size_t(copy_len));
    if (buf != nullptr && buf_size > 0) buf[copy_len] = '\0';
    return copy_len;
}

void set_clipboard_text(const char* text) {
    if (text == nullptr) return;
    GlobalCtx* g = global();
    if (g == nullptr) return;
    g->state->set_clipboard_text(text);
}

const imgui_quic_core_api_t kCoreApi = {
    backend_create,
    backend_destroy,
    backend_new_frame,
    backend_alloc_texture_id,
    backend_poll_event,
    begin_frame,
    add_draw_list,
    end_frame,
    send_texture,
    get_clipboard_text,
    set_clipboard_text,
};

}  // namespace
}  // namespace imgui_quic_core

// The shared ImGui backend (src/imgui_backend.cpp) is compiled into this
// library verbatim.
extern "C" {
void imgui_quic_imgui_backend_set_core_api(const imgui_quic_core_api_t* api);
bool imgui_quic_imgui_backend_init(void);
void imgui_quic_imgui_backend_shutdown();
void imgui_quic_imgui_backend_begin_frame();
void imgui_quic_imgui_backend_render();
}

extern "C" int imgui_quic_init(const imgui_quic_config_t* config) {
    {
        // Serialize init start: concurrent calls must not create two contexts.
        std::lock_guard<std::mutex> lk(imgui_quic_core::g_mutex);
        if (imgui_quic_core::g_initialized.load(std::memory_order_seq_cst) ||
            imgui_quic_core::g_initializing) {
            fprintf(stderr, "[imgui_quic] Already initialized\n");
            return 0;
        }
        imgui_quic_core::g_initializing = true;
    }
    auto initializing_done = [&] {
        std::lock_guard<std::mutex> lk(imgui_quic_core::g_mutex);
        imgui_quic_core::g_initializing = false;
    };

    auto state = std::make_shared<imgui_quic_core::State>();

    imgui_quic_core::ClientLimits limits;
    if (config != nullptr) {
        limits.max_clients = config->max_clients;
        limits.max_clients_per_ip = config->max_clients_per_ip;
    }
    state->set_client_limits(limits);

    {
        std::lock_guard<std::mutex> lk(imgui_quic_core::g_mutex);
        imgui_quic_core::g_global =
            std::make_unique<imgui_quic_core::GlobalCtx>(imgui_quic_core::GlobalCtx{state});
    }


    imgui_quic_imgui_backend_set_core_api(&imgui_quic_core::kCoreApi);
    if (!imgui_quic_imgui_backend_init()) {
        {
            std::lock_guard<std::mutex> lk(imgui_quic_core::g_mutex);
            imgui_quic_core::g_global.reset();
        }
        initializing_done();
        return -3;
    }
    imgui_quic_core::g_backend_initialized.store(true, std::memory_order_seq_cst);
    imgui_quic_core::g_initialized.store(true, std::memory_order_seq_cst);
    initializing_done();

    fprintf(stderr, "[imgui_quic] Initialized native I/P core\n");
    return 0;
}

extern "C" int imgui_quic_start(const imgui_quic_transport_config_t* config) {
    if(!config)return -1;
    try {
        std::lock_guard<std::mutex> lock(imgui_quic_core::g_mutex);
        auto* g=imgui_quic_core::g_global.get();
        if(!g || !imgui_quic_core::g_initialized.load() || g->quic)return -1;
        g->quic=imgui_quic_core::start_quic(g->state,*config);
        if(!g->quic)return -2;
        return 0;
    }catch(...){return -3;}
}

extern "C" void imgui_quic_shutdown() {
    if (!imgui_quic_core::g_initialized.load(std::memory_order_seq_cst)) return;
    if (imgui_quic_core::g_backend_initialized.exchange(false, std::memory_order_seq_cst)) {
        imgui_quic_imgui_backend_shutdown();
    }
    {
        std::lock_guard<std::mutex> lk(imgui_quic_core::g_mutex);
        imgui_quic_core::g_global.reset();
    }
    imgui_quic_core::g_initialized.store(false, std::memory_order_seq_cst);
    fprintf(stderr, "[imgui_quic] Shutdown complete\n");
}

extern "C" void imgui_quic_new_frame() {
    imgui_quic_imgui_backend_begin_frame();
}

extern "C" void imgui_quic_render() { imgui_quic_imgui_backend_render(); }

extern "C" void imgui_quic_media_get(imgui_quic_media_info* info) {
    if(!info)return;
    *info={};
    if(auto* g=imgui_quic_core::global())*info=g->state->get_media();
}
extern "C" void imgui_quic_media_control(int action,float value) {
    if(auto* g=imgui_quic_core::global())g->state->control_media(action,value);
}

extern "C" uint64_t imgui_quic_texture_upload_rgba(uint64_t id, uint32_t width,
    uint32_t height, const uint8_t* pixels, size_t byte_count) {
    if(!pixels || !width || !height || width>16384 || height>16384)return 0;
    const uint64_t expected=uint64_t(width)*height*4;
    if(expected>64*1024*1024 || expected!=byte_count)return 0;
    std::lock_guard<std::mutex> lock(imgui_quic_core::g_mutex);
    auto* g=imgui_quic_core::g_global.get();
    if(!g || !imgui_quic_core::g_initialized.load())return 0;
    if(id==0) {
        if(g->next_app_texture_id>=IMGUI_QUIC_MEDIA_TEXTURE_ID)return 0;
        id=g->next_app_texture_id++;
    } else if(id<UINT64_C(2147483648) || id>=g->next_app_texture_id) return 0;
    g->state->send_texture(id,imgui_quic_core::make_texture_msg(id,width,height,pixels,(uint32_t)byte_count));
    return id;
}
