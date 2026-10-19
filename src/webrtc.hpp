#pragma once
#include "core.hpp"
#include "imgui_quic_webrtc.h"
namespace imgui_quic_core {
struct WebRtcHandle;
std::shared_ptr<WebRtcHandle> start_webrtc(std::shared_ptr<State>, const imgui_quic_webrtc_config_t&);
}
