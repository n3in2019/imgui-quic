# Third-Party Notices

ImGuiQuic includes generated source and metadata derived from these projects:

## Dear ImGui

- Project: https://github.com/ocornut/imgui
- License: MIT
- Copyright: Copyright (c) 2014-2024 Omar Cornut

The CMake configure step downloads and builds pinned Dear ImGui docking
revision `b61e56346a92cfcaf1f43a545ca37b0b32239654` (version 1.92.8) when
`third_party/imgui` is not already present. This matches the
version used for the committed generated bindings. If redistributed with Dear
ImGui sources or binaries, keep the Dear ImGui MIT license notice.

## LZ4

- Project: https://github.com/lz4/lz4
- License: BSD-2-Clause (library)
- Copyright: Copyright (C) 2011-2023, Yann Collet

The core builds LZ4 1.10.0 at revision
`ebb370ca83af193212df4dcbadcc5d87bc0de2f0`, fetched into `third_party/lz4`.
Redistribute its license, installed as `share/doc/imgui_quic/LZ4_LICENSE`, with
source or binary distributions containing the core. The browser implements the
raw block decoder directly and does not bundle upstream LZ4 JavaScript code.

## dear_bindings

- Project: https://github.com/dearimgui/dear_bindings
- License: MIT

`include/imgui_quic_imgui.h`, `src/imgui_quic_imgui.cpp`, and the
JSON metadata under `tools/dear_bindings/` are generated with
`dear_bindings` revision `c9ff64913915df41c0f4beef485b98a1c685eda5`.
If regenerated or redistributed, keep applicable dear_bindings and Dear ImGui
notices.

## Native QUIC / WebTransport

The native endpoint links [Google QUICHE](https://github.com/google/quiche)
(BSD-3-Clause) at revision `121d3bf39d6c4bbf324a4e1f45f92de0e68387d7`.
`libimgui_quic_quiche.so` includes its dependencies, including BoringSSL
(multiple BSD/ISC/OpenSSL notices), Abseil (Apache-2.0), Protocol Buffers
(BSD-3-Clause), and other dependencies resolved by upstream `MODULE.bazel.lock`.
System libraries such as ICU may remain dynamically linked.

`transport/quiche/dependencies.json` pins the source revision and Bazel 8.2.1
binary hashes for Linux x86-64 and AArch64. The build preserves upstream source
licenses and collects fetched dependency license/notice files into
`QUIC_THIRD_PARTY_NOTICES.txt`, installed under `share/doc/imgui_quic`.
Redistribute that notice file with `libimgui_quic_quiche.so`.

## Development and benchmark dependencies

The optional C++ development launcher links system OpenSSL libcrypto to prepare
local certificates; preserve the notices for the redistributed OpenSSL version.
aioquic 1.3.0 (BSD-3-Clause) is an independent test and benchmark client and is not
linked into or required by the running native server. Retain their upstream
notices when redistributing a development environment.
