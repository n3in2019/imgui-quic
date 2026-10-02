#pragma once
#include "core.hpp"
#include "imgui_quic_transport.h"
namespace imgui_quic_core {
struct QuicHandle;
std::shared_ptr<QuicHandle> start_quic(std::shared_ptr<State> state, const imgui_quic_transport_config_t& config);
}
