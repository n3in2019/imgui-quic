# AGENTS.md — ImGuiQuic

## Build and test

```bash
cmake -B build -DIMGUI_QUIC_BUILD_EXAMPLES=ON -DIMGUI_QUIC_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
node tests/browser/draw_ip.mjs
node tests/browser/compressed_draw.mjs
node tests/browser/clipboard_shortcuts.mjs
node tests/browser/large_clipboard_batch.mjs
node tests/browser/media_viewer.mjs
./build/examples/imgui_quic_dev
```

Run `./build/examples/imgui_quic_dev` and open the
URL in `build/webtransport_dev/url.txt` for visual verification. The launcher serves `frontend/` over HTTP. CMake fetches pinned Dear
ImGui into ignored `third_party/`. Requires C++17, CMake 3.21+, Git;
frontend tests require Node 22+.

## Architecture

- `src/`: State/input scheduling, native ImGui backend and I/P
  draw-data transport. `src/draw_protocol.*` serializes geometry and deltas.
- `frontend/`: WebGL renderer, exact I/P decoding, input, clipboard and media.
  The development launcher serves these files; reload the page after edits.
- `include/imgui_quic.h`: small language-neutral lifecycle API only.
- `include/imgui_quic.hpp`: thin C++ wrapper. Applications call native ImGui;
  do not add API interception or duplicate the ImGui runtime.
- `include/imgui_quic_imgui.h`, `src/imgui_quic_imgui.cpp`,
  `tools/dear_bindings/*.json`: committed generated native C API and metadata.
  Never handwrite broad wrappers; regenerate using
  `python3 tools/dear_bindings/generate.py`, then rebuild.
- Upstream bump: `python3 tools/bump_upstream.py <imgui-tag-or-sha>` updates
  cmake/Dependencies.cmake, generator and port pins, regenerates bindings, builds and runs CTest.

## Draw wire protocol

Little-endian. Capability hello `0x0a` carries magic IMGW and server bits:
0 draw data, 4 I/P required, 5 motion optional, 6 fine quantization, 7 LZ4,
8 quarter quantization, 9 integer quantization, 10 planar subtraction/LZ4. Client assignment `0x06`
contains a u32 ID. Capability ACK `0x1a` includes client ID then u32 bits;
it must lead its input record, and trailing early input is discarded.
Clients must advertise the I/P capability.

I frame `0x0d`: id u32, baseline 0 u32, decoded length u32, full geometry.
P frame `0x0e`: same header, acknowledged baseline ID, then sorted nonoverlapping
patches (offset u32, byte count u32, bytes). Copy baseline, resize, apply patches.
Motion P `0x0f`: same header, motion count u32, then list index u32/dx f32/dy f32
records, followed by residual patches. Only position floats are translated;
residual patches restore exact original bytes. Choose motion only if smaller.
Compressed I `0x22` / P `0x23`: same 13-byte header, then a raw LZ4 block.
I uses baseline zero; P compresses current XOR acknowledged baseline, with zero
extension and output truncation. Declared decoded length remains bounded to
16 MiB. Tiny exact packets (<=512 bytes) skip compression; larger updates choose
between LZ4 and bounded exact encoding. Compressed I stays reliable/single-flight.
Both native and browser transport dispatch must recognize the compressed types.
Run `node tests/browser/compressed_draw.mjs`; preserve exact legacy negotiation.
Planar I `0x26` / P `0x27`: 13-byte header, lane width u8 (12/20), raw LZ4
of lane-transposed modulo-256 byte subtraction. Undo lanes and add the ACKed
baseline. Verify width matches restored geometry. I stays reliable/single-flight.
Try only for bulk packets (>=4096 bytes); stable position/UV samples skip it.
Run `node tests/browser/planar_draw.mjs`; preserve resource/epoch/ACK invariants.

Canonical geometry starts with `0x01`, six f32 values (DisplayPos, DisplaySize,
FramebufferScale), list count u32. Each list has vertex/index/command counts u32,
20-byte vertices (pos, UV, color), u32 indices, and 36-byte commands (clip 4*f32,
texture u64, index offset u32, vertex offset u32, element count u32).
Native renderer callbacks are not transported.

Packed `0x21`/`0x24`/`0x25` geometry uses the same frame header, then counts, two f32
origins, 12-byte vertices (i16 x/y at 1/16, 1/4 or integer units, u16 UVs, RGBA8), and
unchanged indices/commands. Keep packed bytes as ACK baselines; expand a separate
canonical buffer for rendering. Whole-frame fallback preserves unsupported
ranges. Browser default is integer; `draw-precision=exact|fine|quarter|integer`
selects precision. Only request advertised bits, with fine/exact legacy fallback.
Run `node tests/browser/quantized_draw.mjs` after changing this path.

Client `0x1d` + client ID u32 + frame ID u32 acknowledges a rendered frame;
ID zero requests recovery textures and an I frame. Never reference an
unacknowledged baseline. Max decoded frame 16 MiB, outstanding window 8,
client cache 9. Latest unsent frame may replace its predecessor. Texture/control
prerequisites must be drained before frames. Identical geometry is suppressed
except heartbeat or texture updates. Bootstrap/recovery I frames are single-flight;
in-flight encoded frames have a 24 KiB budget (one oversized frame may proceed
alone). Stalled ACKs disconnect after 10 seconds, or 60 seconds before the first
baseline ACK to allow resource initialization.

Texture `0x02`: ID u64, width/height/byte length u32, RGBA8 pixels. Textures
persist until shutdown and latest pixels are resent on reconnect. Application
texture IDs start at 2^31. `imgui_quic_texture.h` copies uploads, max 64 MiB.

## Input and multi-client invariants

All client input includes type u8 then client ID u32:
`0x10` mouse x/y f32; `0x11/12` button down/up u8; `0x13` wheel dx/dy f32;
`0x14/15` key down/up u16; `0x16` Unicode character u32; `0x17` CSS width/height
f32 plus optional device scale f32; `0x18` clipboard length u32 and UTF-8.
Server clipboard `0x18` omits client ID.

Clients share one native ImGui context. The active client determines layout;
other canvases display scaled geometry. Preserve global input FIFO. A client
with a different size requires a preparation layout before positional input.
Freeze viewport through that frame and ImGui event-trickling frames.
Changed mouse position must be submitted for one frame before the next button
edge: AllowOverlap widgets depend on previous-frame hover ownership. Stationary
position records add no delay. Finish trickling existing events before adding
new edges. Tests must not hide scheduling errors with hover sleeps.

## Media

`imgui_quic_media.h` stays separate from lifecycle. Browser-local media texture
ID is 4294967294. Files and decoded pixels stay local. Client `0x1b` after ID:
flags u32 (kind 0..2, paused=4, muted=8, error=16), width/height u32,
position/duration/volume f32, message length u32 and UTF-8 (max 1023).
Server `0x1c`: action u8, value f32. Controls target the active browser.
Closing the viewer releases browser resources. Server sample image uses the
separate texture upload/update API.

## Hygiene

Keep LICENSE, README, CONTRIBUTING, SECURITY and THIRD_PARTY_NOTICES current.
Preserve generated/upstream MIT notices. Never commit build/, .venv/ or
third_party/. Rebuild after edits to src/, include/ or frontend/. Run relevant
regressions and verify end-to-end after behavior changes.

## Native WebTransport

`transport/quiche/` builds a pinned Google QUICHE C++ shared library with BoringSSL.
`src/quic.cpp` connects it directly to `State`.
`include/imgui_quic_transport.h` adds a separate transport-start API; main lifecycle
shutdown stops and joins native QUIC before releasing state. Clang/C++20 and ICU development files are
required by the default Linux x86-64/AArch64 build; Bazel is downloaded with a pinned hash. The launcher prepares credentials, serves frontend assets and supervises
the native example.

Run `node tests/browser/webtransport.mjs`,
`ctest --test-dir build --output-on-failure`,
and `build/webtransport_venv/bin/python tests/webtransport/test_native.py` after
transport changes. Install test requirements and prepare the development cert as
explained in tools/webtransport/README.md. Preserve monotonic presentation/ACKs,
resource barriers, epoch recovery and pointer fences. Never ACK on receipt or
accept an ACK for an unsent frame. Keep Origin/token checks and bounded admission.
The browser uses native WebTransport for all interactive sessions.

Native QUIC mailboxes use an immutable notification callback installed by
`State::add_client` before publication. It retains shared eventfd ownership;
never capture a raw transport/runtime pointer in an outbound mailbox callback.
All enqueue/close/recovery paths must notify. QUICHE resumes buffered input/output
budgets immediately; the 100 ms watchdog is only the idle/deadline fallback.
The bridge is private and symbol-versioned: update its ELF version when changing
the by-value callback ABI. Keep the public lifecycle and QUIC APIs unchanged.

## Project entry points

`examples/minimal.cpp` is the starter; `example_draw.cpp` is the full regression
demo. Both use example-only `support.hpp` for process setup. Launch with
`build/examples/imgui_quic_dev --example minimal` or `--example demo`.
Top-level CMake composes modules from `cmake/`; the Dear ImGui pin lives in
`cmake/Dependencies.cmake`. Keep `src/` private to the library and internal tests.
CMake 3.21+ users can use the `dev` and `core-tests` presets.
