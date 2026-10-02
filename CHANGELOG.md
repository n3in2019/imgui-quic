# Changelog

## Unreleased

- Use ImGuiQuic branding, `imgui_quic` C/C++ and CMake identifiers, and the `imgui-quic` repository/port name.
- Build the QUICHE adapter and collect dependency notices using CMake.
- Prepare development credentials and launch examples with a C++ tool using the system OpenSSL CLI.

Initial native WebTransport implementation:

- Native Dear ImGui context, docking and generated C bindings.
- Exact acknowledged I/P draw frames, motion deltas and bounded recovery.
- In-process Google QUICHE and BoringSSL for Linux x86-64/AArch64.
- Browser WebGL renderer, ordered input, clipboard, textures and local media.
- QUIC-only native process with separately hosted frontend; token/Origin checks, admission limits and graceful shutdown.
- Event-driven transport notifications and bounded encoding work.
- Minimal starter, interactive demo, CMake package and development presets.
- Native, frontend and QUIC interoperability tests and netem benchmarks.
