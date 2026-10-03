# Remote ImGui benchmarks

The harness runs the production ImGuiQuic encoder and pinned imgui-ws, netImgui
and RemoteImGui encoders against the same Dear ImGui scenes. It measures encoding
cost, payload size, independent decoding, native QUIC delivery and server CPU.
[Encoding results](../../../docs/benchmarks/packed-live/README.md),
[SIMD validation](../../../docs/benchmarks/sse2-integration/README.md) and
[server CPU results](../../../docs/benchmarks/server-cpu/README.md) describe the
measurement scope and limits.

## Build and run

Configure the main project to fetch pinned Dear ImGui and LZ4. Clone the URLs in
[upstream.json](upstream.json) into `build/comparison/upstream/<name>` and check
out each recorded revision. Python is required only for these optional benchmark
and adapter tools. Build the comparison executable:

```bash
cmake -S tools/benchmarks/comparison -B build/comparison/build -DCMAKE_BUILD_TYPE=Release
cmake --build build/comparison/build -j6
python3 tools/benchmarks/comparison/run.py --output build/comparison/results.json
node tools/benchmarks/comparison/decode.mjs build/comparison/decode.json
```

The runner checks dependency revisions, pins itself and its children to one
logical CPU, and randomizes trial order with seed 42. Defaults are five trials,
900 measured frames and 30 warm-up frames per scene. Run timings separately from
builds and other benchmarks. Source hashes, dependency revisions, host/compiler
metadata and individual trials are retained in the result file.

Viewport is 1280×720, window size 1000×650, and simulation step 1/30 second.
Scenes cover static text, moving windows, changing text, 4096-point plots,
32×8 tables, changing triangle topology and 6000 colored triangles. Each codec
starts with a new ImGui context; layout is outside the encoding timer.

The timer includes serialization, quantization, allocation, candidate evaluation,
compression selection and previous-frame ownership updates. It excludes network,
texture bootstrap, input, decode and GPU work. Static frames are deliberately
encoded on every call; production geometry suppression is outside this harness.
Immediate previous-frame availability models zero ACK delay. `first_bytes` is a
UI frame, not total connection/bootstrap traffic. Bytes/frame multiplied by 30
is a hypothetical application payload rate, not measured bandwidth.

## Production codec modes

| Algorithm | Geometry and compression |
|---|---|
| `imgui-quic-q1-adaptive-live` | Browser default: integer offsets, u16 UVs, exact/XOR/planar selection |
| `imgui-quic-q4-adaptive-live` | Quarter-unit offsets with the same selection |
| `imgui-quic-q16-adaptive-live` | Fine 1/16-unit offsets with the same selection |
| `imgui-quic-adaptive-live` | Float geometry with exact/XOR/planar selection |
| `imgui-quic-adaptive` | Float geometry with exact/XOR selection |
| `imgui-quic` | Float geometry with exact patches/motion |
| `imgui-quic-q1`, `-q4`, `-q16` | Selected packed geometry with exact patches |
| `raw` | Canonical geometry serialization only |

All ImGuiQuic modes use the current library implementation. The `live` suffix
selects negotiated planar capability in the encoder; it does not open a network
connection. Codec selection and both compression candidates are inside timing.

A forced-scalar build evaluates SIMD using the same source and wire protocol:

```bash
cmake -S tools/benchmarks/comparison -B build/comparison/scalar -DCMAKE_BUILD_TYPE=Release -DIMGUI_QUIC_FORCE_SCALAR_QUANTIZATION=ON
cmake --build build/comparison/scalar -j6
python3 tools/benchmarks/comparison/cpu.py --before build/comparison/scalar/codec --after build/comparison/build/codec --before-source src/draw_protocol.cpp --output build/comparison/cpu.json
```

`cpu.py` requires byte-identical fixtures across four precision modes before
collecting randomized CPU timings. `decode.mjs` uses the production JavaScript
decoder, includes validation/expansion/cache commit in timing, and checks metadata
and coordinate/UV error outside timing. Its five Node/V8 trials use 120 measured
frames per scene after warm-up. WebGL/compositor and actual browser presentation
are excluded; GC and case order can affect results. Cache bytes and expanded-frame
bytes are reported separately.

## Upstream adapters and fidelity

- **imgui-ws:** upstream `XorRlePerDrawListWithVtxOffset`; only `TextureId` is
  adapted to `GetTexID()` for Dear ImGui 1.92. Float positions are relative to the
  first vertex, with 16-bit indices and 32-bit texture identifiers. `diffSize()`
  excludes WebSocket/incppect framing and traffic.
- **netImgui:** upstream conversion and command compression, including command
  headers. Its u16 positions cover 16384 units at approximately 0.25-unit steps;
  UVs are u16. Compression preserves this packed representation.
- **RemoteImGui:** `prepare_remote.py` extracts serialization, byte differences
  and the upstream bundled LZ4 into a network-free adapter with symbol prefixes.
  Socket sends become byte counting; its application/viewer is not compiled.
  Positions are signed integers, UVs fixed-point and indices 16-bit. The adapter
  encodes every supplied frame with a keyframe every 60 calls, omitting the
  upstream application's three-frame throttle. LZ4 chunk headers are included;
  WebSocket headers are excluded. These are adapted codec measurements, not its
  application's frame rate.

ImGuiQuic's nearest relative offsets and retained float origins differ from
upstream quantization. Payload and CPU figures are not equal-fidelity or
full-product rankings. Upstream source/licenses stay in ignored build directories;
the adapter does not vendor their implementations into this repository.

For independent imgui-ws and netImgui checks, run
`build/comparison/build/codec <algorithm> <scene> 120 verify`. Verification runs
must remain separate from timing. Production CTest and browser tests validate
ImGuiQuic's negotiated formats, malformed data, recovery and presentation ACKs.

## Native network and server CPU

Build the main project with `IMGUI_QUIC_BUILD_BENCHMARKS=ON`, prepare credentials
and the Python receiver environment using [the transport guide](../README.md).
Linux `ip`, `tc` and unprivileged user/network namespaces are required for network
shaping; the runners refuse the host network namespace.

`packed_network.py` compares exact XOR/LZ4 with integer/planar delivery over native
QUIC. `network.py` compares QUICHE with the actual imgui-ws/incppect/uWebSockets/
uSockets stack. For the latter, fetch imgui-ws submodules recursively, configure
`BUILD_UPSTREAM_WS_HOST=ON`, and install `websockets==14.2` in the receiver
Python environment. The WebSocket host is unencrypted; QUIC includes TLS, so
server CPU includes different security costs. Other upstream viewers/stacks are
not measured. The imgui-ws receiver polls at 30 Hz without per-message compression;
its variable updates do not guarantee an atomic whole-frame snapshot.

```bash
IMGW_BENCH_HOST_NETNS="$(readlink /proc/self/ns/net)" unshare --user --map-root-user --net build/webtransport_venv/bin/python tools/benchmarks/comparison/packed_network.py --output build/comparison/network.json
```

Receivers decode timestamp and consumed-input markers and ACK decoded frames.
Age is generation-to-decode, not GPU presentation. Setup/fonts are excluded.
Python receive/decode can limit delivered FPS; retain its cost alongside server
CPU. Wire rates include QUIC overhead and retransmissions; delivering more frames
can increase throughput despite smaller frames. `fanout.py` and `server_cpu.py`
measure independent 1/4/8-client receivers; the benchmark host alone permits eight
clients per address. CPU is percent of one logical CPU across server threads and
excludes receivers. RSS sampling and test duration are documented in each report.
