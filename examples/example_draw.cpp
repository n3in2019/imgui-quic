// Native Dear ImGui example: acknowledged I/P draw data over native WebTransport.

#include <chrono>
#include <csignal>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "support.hpp"
#include "imgui.h"
#include "imgui_quic.hpp"
#include "imgui_quic_transport.h"
#include "imgui_quic_media.h"
#include "imgui_quic_texture.h"

// Opt-in geometry observations for protocol-driven tests. They locate targets;
// tests still click/hover via WebTransport and assert the rendered UI outcomes.
static void debug_item_rect(const char* name) {
    if (!std::getenv("IMGW_GEOM_DEBUG") || ImGui::GetFrameCount() % 10 != 0) return;
    const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    std::fprintf(stderr, "[item] %s|%.1f|%.1f|%.1f|%.1f\n", name, min.x, min.y, max.x, max.y);
}

int main(int argc, char** argv) {
    imgui_quic::Server app;
    if (!example::start(app, argc, argv)) return 1;

    // Keyboard navigation and docking run on the native host.
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;

    // --- example state -------------------------------------------------------
    float f = 0.0f;
    int counter = 0;
    bool show_demo = false;
    bool show_inspector = false;
    bool show_settings = false;
    bool running = true;
    bool show_details = false;   // modal submitted while true (p_open-owned)
    bool details_request = false; // OpenPopup must run in the main window scope
    int combo_item = 0;
    const char* combo_items[] = {"Alpha", "Beta", "Gamma", "Delta"};
    int list_selected = 1;
    const char* list_items[] = {"first", "second", "third", "fourth"};
    int tab_track = 0;
    float col3[3] = {0.4f, 0.7f, 0.9f};
    char text_buf[128] = "edit me";
    static int details_clicks = 0;

    // Application-owned pixels: uploaded once by C++, shared with every
    // browser and retained by the server for clients which join later.
    std::vector<uint8_t> image_pixels(320*180*4);
    for(unsigned y=0;y<180;y++)for(unsigned x=0;x<320;x++) {
        const unsigned p=(y*320+x)*4;
        image_pixels[p]=uint8_t(x*255/319);
        image_pixels[p+1]=uint8_t(y*255/179);
        image_pixels[p+2]=((x/20+y/20)&1)?220:50;
        image_pixels[p+3]=255;
    }
    const uint64_t server_image=imgui_quic_texture_upload_rgba(0,320,180,image_pixels.data(),image_pixels.size());
    bool show_server_image=false;
    auto main_render = app.on_render([&]() {
        // Submit the dockspace before any window it may host.
        ImGuiID dockspace_id = ImGui::DockSpaceOverViewport();

        // --- Menu bar (BeginMenuBar/BeginMenu scopes) -----------------------
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Open Details", "Ctrl+D")) details_request = true;
                if (ImGui::MenuItem("Inspector")) show_inspector = true;
                ImGui::Separator();
                if (ImGui::MenuItem("Quit", "Alt+F4")) running = false;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
                ImGui::MenuItem("Demo Window", nullptr, &show_demo);
                ImGui::MenuItem("Inspector", nullptr, &show_inspector);
                ImGui::MenuItem("Settings", nullptr, &show_settings);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Help")) {
                if (ImGui::MenuItem("About")) {}
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        if (show_demo) {
            ImGui::ShowDemoWindow(&show_demo);
        }

        // Submit the modal using the ordinary ImGui popup pattern.
        if (details_request) {
            details_request = false;
            ImGui::OpenPopup("Details");
            show_details = true;
        }
        if (ImGui::BeginPopupModal("Details", &show_details,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Details popup survives close/reopen cycles");
            ImGui::Text("details clicks: %d", details_clicks);
            const bool close_details = ImGui::Button("Close");
            debug_item_rect("modal-close");
            if (close_details) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Initial layout: dock the example window into the central node once
        // (FirstUseEver); subsequent layout changes appear in each draw frame.
        ImGui::SetNextWindowDockID(dockspace_id, ImGuiCond_FirstUseEver);
        ImGui::Begin("ImGuiQuic Example (C++ core)", &running);
        ImGui::TextUnformatted("Hello from the pure-C++ core!");
        ImGui::Checkbox("Show Demo Window", &show_demo);
        debug_item_rect("demo-toggle");
        ImGui::SliderFloat("float", &f, 0.0f, 1.0f);
        ImGui::ColorEdit3("clear color", col3);
        if (ImGui::Button("Button")) {
            counter++;
        }
        ImGui::SameLine();
        {
            char label[64];
            snprintf(label, sizeof(label), "counter = %d", counter);
            ImGui::TextUnformatted(label);
        }
        ImGui::InputText("text", text_buf, sizeof(text_buf));
        ImGui::Separator();

        // Keyboard shortcut P toggles a popup.
        {
            static bool key_popup_open = false;
            const bool key_pressed = ImGui::IsKeyPressed(ImGuiKey_P);
            if (key_pressed && !key_popup_open) {
                key_popup_open = true;
                ImGui::OpenPopup("keyboard popup");
            }
            if (ImGui::BeginPopup("keyboard popup")) {
                ImGui::TextUnformatted("Keyboard popup rendered on the native host");
                if (ImGui::Button("Close##kb")) {
                    ImGui::CloseCurrentPopup();
                    key_popup_open = false;
                }
                ImGui::EndPopup();
            } else if (key_popup_open) {
                key_popup_open = false;  // closed otherwise (e.g. Esc)
            }
        }

        if (ImGui::BeginTabBar("main tabs")) {
            if (ImGui::BeginTabItem("Basics")) {
                // Standard combo popup.
                if (ImGui::BeginCombo("combo", combo_items[combo_item])) {
                    for (int n = 0; n < 4; n++) {
                        const bool selected = (combo_item == n);
                        if (ImGui::Selectable(combo_items[n], selected)) combo_item = n;
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                // List box (child-window scope).
                if (ImGui::BeginListBox("listbox")) {
                    for (int n = 0; n < 4; n++) {
                        const bool selected = (list_selected == n);
                        if (ImGui::Selectable(list_items[n], selected)) list_selected = n;
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndListBox();
                }
                // Item tooltip (conditional EndTooltip pairing).
                ImGui::Button("hover me");
                if (ImGui::BeginItemTooltip()) {
                    ImGui::TextUnformatted("a remote item tooltip");
                    ImGui::EndTooltip();
                }
                // Context popup on an item (BeginPopupContextItem, explicit
                // str_id: Text items have no implicit id to derive it from).
                ImGui::TextUnformatted("right-click me (item context popup)");
                if (ImGui::BeginPopupContextItem("item ctx")) {
                    if (ImGui::MenuItem("item: copy")) counter++;
                    if (ImGui::MenuItem("item: delete")) counter--;
                    ImGui::EndPopup();
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Scopes")) {
                // Programmatically opened popup.
                if (ImGui::Button("Open test popup")) ImGui::OpenPopup("popup test");
                if (ImGui::BeginPopup("popup test")) {
                    ImGui::TextUnformatted("Popup state lives on the server");
                    if (ImGui::Button("close##popup")) ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
                // Window context popup (explicit str_id variant).
                if (ImGui::Button("Open window context popup"))
                    ImGui::OpenPopup("window ctx");
                if (ImGui::BeginPopupContextWindow("window ctx")) {
                    ImGui::TextUnformatted("window context popup");
                    if (ImGui::MenuItem("close##winctx")) ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
                // Void context popup: opens when clicking empty space.
                if (ImGui::BeginPopupContextVoid()) {
                    if (ImGui::MenuItem("void: surprise")) counter += 10;
                    ImGui::EndPopup();
                }
                // Nested scope stress: popup inside popup.
                if (ImGui::Button("Open nested popups")) ImGui::OpenPopup("outer");
                if (ImGui::BeginPopup("outer")) {
                    ImGui::TextUnformatted("outer");
                    if (ImGui::Button("open inner")) ImGui::OpenPopup("inner");
                    if (ImGui::BeginPopup("inner")) {
                        ImGui::TextUnformatted("inner");
                        ImGui::EndPopup();
                    }
                    ImGui::EndPopup();
                }
                ImGui::Text("tab tracker: %d", tab_track);
                if (ImGui::Button("bump tracker")) tab_track++;
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Table")) {
                if (ImGui::BeginTable("table", 3, ImGuiTableFlags_Borders)) {
                    for (int row = 0; row < 4; row++) {
                        ImGui::TableNextRow();
                        for (int col = 0; col < 3; col++) {
                            ImGui::TableSetColumnIndex(col);
                            ImGui::Text("cell %d/%d", row, col);
                        }
                    }
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Widgets")) {
                // One representative of every remaining widget family, so the
                // e2e battle harness can drive each over the real protocol.
                // Formatted text (printf varargs): formatted on the host.
                ImGui::Text("formatted text %d/%d", 1, 2);
                ImGui::BulletText("bullet text %.2f", 0.25f);
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "colored %s", "text");
                ImGui::TextDisabled("disabled %d", 7);
                ImGui::SeparatorText("inputs");
                static float drag_f = 0.5f;
                static int drag_i = 3;
                ImGui::DragFloat("drag float", &drag_f, 0.01f);
                ImGui::DragInt("drag int", &drag_i);
                static int slider_i = 2;
                ImGui::SliderInt("slider int", &slider_i, 0, 10);
                static int input_i = 7;
                static float input_f = 1.25f;
                ImGui::InputInt("input int", &input_i);
                ImGui::InputFloat("input float", &input_f);
                ImGui::Text("drag=%.3f di=%d si=%d ii=%d if=%.2f",
                            drag_f, drag_i, slider_i, input_i, input_f);
                static char multi_buf[128] = "multiline default";
                static char hint_buf[128] = "hinted default";
                ImGui::InputTextMultiline("multiline", multi_buf, sizeof(multi_buf));
                ImGui::InputTextWithHint("hinted", "enter text", hint_buf, sizeof(hint_buf));
                static float col4[4] = {0.2f, 0.4f, 0.6f, 1.0f};
                ImGui::ColorEdit4("color4", col4);
                static int radio = 0;
                ImGui::RadioButton("radio a", &radio, 0); ImGui::SameLine();
                ImGui::RadioButton("radio b", &radio, 1); ImGui::SameLine();
                static int geom_tick = 0;  // IMGW_GEOM_DEBUG layout calibration
                if (getenv("IMGW_GEOM_DEBUG") && (geom_tick++ % 60) == 0) {
                    fprintf(stderr, "[geom] radio-b=(%.0f,%.0f)-(%.0f,%.0f)\n",
                            ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y,
                            ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
                }
                ImGui::RadioButton("radio c", &radio, 2);
                static unsigned flags_u = 0;
                ImGui::CheckboxFlags("flags A", &flags_u, 1u); ImGui::SameLine();
                ImGui::CheckboxFlags("flags B", &flags_u, 2u);
                static int combo_idx = 0;
                const char* combo_arr[] = {"one", "two", "three"};
                ImGui::Combo("combo arr", &combo_idx, combo_arr, 3);
                // Temporary layout calibration for the e2e harness probes.
                if (getenv("IMGW_GEOM_DEBUG") && (geom_tick % 60) == 0) {
                    fprintf(stderr, "[geom] combo=(%.0f,%.0f)-(%.0f,%.0f)\n",
                            ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y,
                            ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
                }
                const bool tree_open = ImGui::TreeNode("tree node");
                debug_item_rect("tree");
                if (tree_open) {
                    ImGui::TextUnformatted("tree node content");
                    ImGui::TreePop();
                }
                if (getenv("IMGW_GEOM_DEBUG") && (geom_tick % 60) == 0) {
                    ImVec2 p = ImGui::GetCursorScreenPos();  // tree row, next frame
                    fprintf(stderr, "[geom] tree-row-top=(%.0f,%.0f) window-scroll=%.0f content-height=%.0f\n",
                            p.x, p.y, ImGui::GetScrollY(), ImGui::GetContentRegionAvail().y);
                }
                const bool header_open = ImGui::CollapsingHeader("collapsing header");
                debug_item_rect("collapsing");
                if (header_open) {
                    ImGui::TextUnformatted("collapsing content");
                }
                // One-shot tooltips (SetTooltip family formats server-side).
                if (ImGui::Button("set tooltip")) {}
                debug_item_rect("set-tooltip");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("set-tooltip %d", 5);
                ImGui::SameLine();
                ImGui::TextUnformatted("item tooltip row");
                debug_item_rect("item-tooltip");
                ImGui::SetItemTooltip("item tooltip %d", 9);
                ImGui::SeparatorText("display");
                static float prog = 0.4f;
                ImGui::SliderFloat("progress value", &prog, 0.0f, 1.0f);
                ImGui::ProgressBar(prog, ImVec2(0.0f, 0.0f), "overlay");
                static float plot_vals[16];
                for (int n = 0; n < 16; n++) {
                    plot_vals[n] = 0.4f + 0.4f * sinf(float(n) * 0.7f);
                }
                ImGui::PlotLines("plot lines", plot_vals, 16);
                ImGui::PlotHistogram("plot hist", plot_vals, 16);
                ImGui::Bullet();
                ImGui::TextUnformatted("bullet + unformatted");
                static int widget_clicks = 0;
                if (ImGui::SmallButton("small")) widget_clicks++;
                ImGui::SameLine();
                if (ImGui::ArrowButton("arrow##widgets", ImGuiDir_Right)) widget_clicks++;
                ImGui::SameLine();
                {
                    char lbl[64];
                    snprintf(lbl, sizeof(lbl), "clicks = %d", widget_clicks);
                    ImGui::TextUnformatted(lbl);
                }
                ImGui::BeginDisabled();
                ImGui::Button("disabled button");
                ImGui::EndDisabled();
                ImGui::Columns(2);
                ImGui::TextUnformatted("column 0");
                ImGui::NextColumn();
                ImGui::TextUnformatted("column 1");
                ImGui::NextColumn();
                ImGui::Columns(1);
                // Drag payload and acceptance remain on the native host.
                static int dnd_payload = 42;
                static int dnd_received = 0;
                static int dnd_deliveries = 0;
                if (ImGui::Button("drag source")) {}
                debug_item_rect("drag-source");
                if (ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload("WIDGETS_DND", &dnd_payload, sizeof(int));
                    ImGui::TextUnformatted("dragging");
                    ImGui::EndDragDropSource();
                }
                ImGui::SameLine();
                if (ImGui::Button("drop target")) {}
                debug_item_rect("drop-target");
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("WIDGETS_DND")) {
                        dnd_received = *(const int*)pl->Data;
                        ++dnd_deliveries;
                    }
                    ImGui::EndDragDropTarget();
                }
                ImGui::SameLine();
                {
                    char lbl[64];
                    snprintf(lbl, sizeof(lbl), "received = %d, drops = %d", dnd_received, dnd_deliveries);
                    ImGui::TextUnformatted(lbl);
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Media")) {
                if(ImGui::Button("Sample image")) {show_server_image=true;imgui_quic_media_control(IMGUI_QUIC_MEDIA_CLEAR,0);}
                ImGui::SameLine();
                if(ImGui::Button("Sample video")) {show_server_image=false;imgui_quic_media_control(IMGUI_QUIC_MEDIA_SAMPLE_VIDEO,0);}
                ImGui::TextUnformatted("Or use Open media file, drop a file, or paste an image.");
                ImGui::TextUnformatted("Sample image comes from C++; Open media file previews browser-local files.");
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::Separator();
        ImGui::TextUnformatted("I/P draw-data transport active.");
        ImGui::TextUnformatted("Right-click empty space for the void context popup.");
        ImGui::End();
        if(show_server_image) {
            ImGui::SetNextWindowSize(ImVec2(540,380),ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowPos(ImVec2(180,240),ImGuiCond_FirstUseEver);
            if(ImGui::Begin("Server image",&show_server_image)) {
                ImGui::TextUnformatted("RGBA pixels provided by the C++ server. Shared with all browsers.");
                if(server_image) {
                    const float width=std::fmax(1.0f,std::fmin(ImGui::GetContentRegionAvail().x,640.0f));
                    ImGui::Image(ImTextureRef((ImTextureID)server_image),ImVec2(width,width*180/320));
                } else ImGui::TextUnformatted("Image upload failed.");
                if(ImGui::Button("Update image")) {
                    for(size_t i=0;i<image_pixels.size();i+=4)image_pixels[i]=255-image_pixels[i];
                    imgui_quic_texture_upload_rgba(server_image,320,180,image_pixels.data(),image_pixels.size());
                }
                debug_item_rect("server-image-update");
            }
            ImGui::End();
        }
        imgui_quic_media_info media{};
        imgui_quic_media_get(&media);
        if(media.kind || media.error) {
            ImGui::SetNextWindowSize(ImVec2(540,460),ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowPos(ImVec2(180,240),ImGuiCond_FirstUseEver);
            bool open=true;
            if(ImGui::Begin("Images & video",&open)) {
                ImGui::TextUnformatted(media.message);
                if(media.width && media.height && !media.error) {
                    const float width=std::fmax(1.0f,std::fmin(ImGui::GetContentRegionAvail().x,640.0f));
                    ImGui::Image(ImTextureRef((ImTextureID)IMGUI_QUIC_MEDIA_TEXTURE_ID),ImVec2(width,width*media.height/media.width));
                }
                if(media.kind==2) {
                    if(ImGui::Button(media.paused?"Play":"Pause"))imgui_quic_media_control(media.paused?IMGUI_QUIC_MEDIA_PLAY:IMGUI_QUIC_MEDIA_PAUSE,0);
                    debug_item_rect("media-play");
                    ImGui::SameLine();
                    if(ImGui::Button("Restart"))imgui_quic_media_control(IMGUI_QUIC_MEDIA_SEEK,0);
                    float position=media.position;
                    if(ImGui::SliderFloat("Position (seconds)",&position,0,std::fmax(media.duration,0.001f)))imgui_quic_media_control(IMGUI_QUIC_MEDIA_SEEK,position);
                    float volume=media.volume;
                    if(ImGui::SliderFloat("Volume",&volume,0,1))imgui_quic_media_control(IMGUI_QUIC_MEDIA_VOLUME,volume);
                    bool muted=media.muted;
                    if(ImGui::Checkbox("Muted",&muted))imgui_quic_media_control(IMGUI_QUIC_MEDIA_MUTE,muted?1:0);
                }
                if(ImGui::Button("Clear media"))imgui_quic_media_control(IMGUI_QUIC_MEDIA_CLEAR,0);
                debug_item_rect("media-clear");
            }
            ImGui::End();
            if(!open)imgui_quic_media_control(IMGUI_QUIC_MEDIA_CLEAR,0);
        }


        // --- Inspector: p_open window with close/reopen cycles --------------
        if (show_inspector) {
            ImGui::Begin("Inspector", &show_inspector);
            ImGui::Text("float: %.3f", f);
            ImGui::Text("counter: %d", counter);
            ImGui::Text("combo: %s", combo_items[combo_item]);
            ImGui::Text("list: %s", list_items[list_selected]);
            ImGui::Text("text: %s", text_buf);
            ImGui::ColorButton("col", ImVec4(col3[0], col3[1], col3[2], 1.0f),
                               0, ImVec2(40, 40));
            if (ImGui::Button("Add click")) details_clicks++;
            ImGui::End();
        }

        // --- Settings: checkbox-driven secondary window ----------------------
        if (show_settings) {
            ImGui::Begin("Settings", &show_settings);
            ImGui::Checkbox("Show Demo Window", &show_demo);
            ImGui::Checkbox("Show Inspector", &show_inspector);
            ImGui::SliderFloat("float##settings", &f, 0.0f, 1.0f);
            if (ImGui::Button("Open Details popup")) details_request = true;
            ImGui::End();
        }
    });

    while (running && main_render && !example::stop_requested) {
        app.render();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    return 0;
}
