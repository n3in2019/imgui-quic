# Remote UI encoder comparison

This harness compares ImGuiQuic, imgui-ws, netImgui and RemoteImGui draw encoders
using one pinned Dear ImGui version and the same generated scenes. It measures
serialization plus encoding time and application payload size. For the browser's
default integer/planar encoding, use `imgui-quic-q1-adaptive-live`;
`imgui-quic-adaptive-live` keeps float geometry but enables planar selection.
Both include all candidate evaluation and quantization costs. The separate
`network.py` runner also compares actual QUICHE and imgui-ws network stacks.

## Reproduce

The upstream revisions are recorded in `upstream.json`. Clone each URL into
`build/comparison/upstream/<name>` and check out its recorded commit. Configure
the main project first to fetch its pinned Dear ImGui source, then run:

```bash
cmake -S tools/benchmarks/comparison -B build/comparison/build -DCMAKE_BUILD_TYPE=Release
cmake --build build/comparison/build -j 6
python3 tools/benchmarks/comparison/run.py
```

Results are saved to `build/comparison/results.json`. The runner checks upstream
commit IDs, pins itself and child processes to one available logical CPU, and
randomizes run order with seed 42. Use `--frames` and `--trials` to change defaults.
Run other benchmarks separately to avoid measurement interference.

Each trial renders 30 warmup frames and 900 measured frames without wall-clock
pacing. ImGui's simulation step is 1/30 second, viewport 1280×720, and window
1000×650 with 30 text rows. Scenes are static, sinusoidal integer window movement,
and changing numeric text. `--scenes` additionally accepts `plots` (4096 points),
`tables` (32 rows × 8 columns), `topology` (changing triangle counts), and `dense`
(6000 colored triangles). Each encoder starts from a new ImGui context.
Serialization/encoding is timed; ImGui layout, transport, decoding, textures,
GPU rendering and input handling are excluded. Means include allocations and
previous-frame ownership updates inside the encoder block. `first_bytes` is the
first encoded UI frame, not total connection/bootstrap traffic.

The report uses medians across trials of each trial's mean bytes, mean encoding
time and p95 encoding time. Multiplying bytes/frame by 30 gives a hypothetical
30 FPS application payload rate, not measured network bandwidth. Static results
force an encode every frame: application-level unchanged-frame suppression is
not included. Immediate previous-frame availability assumes zero ACK delay.

## Adapters and fidelity

- **ImGuiQuic:** native canonical serialization and motion-enabled I/P encoder;
  full float32 positions/UVs and u32 indices, preserving exact geometry bytes.
- **imgui-ws:** upstream default `XorRlePerDrawListWithVtxOffset`. The build changes
  only `TextureId` to `GetTexID()` for Dear ImGui 1.92 compatibility. It normalizes
  float positions relative to the first vertex and XOR/RLE encodes each list.
  Its 16-bit indices and 32-bit texture identifiers differ from ImGuiQuic's wire
  layout. `diffSize()` excludes incppect/WebSocket metadata and traffic.
- **netImgui:** upstream `ConvertToCmdDrawFrame` and `CompressCmdDrawFrame`,
  including its command header. Its 12-byte vertex representation quantizes
  positions across a 16384-unit range to u16 (about 0.25-unit steps) and UVs to
  u16. Compression preserves that quantized representation, not original floats.
- **RemoteImGui:** `prepare_remote.py` extracts the upstream serialization,
  byte-difference and LZ4 routines into a network-free adapter. It links the
  upstream bundled LZ4 with generated symbol prefixes for isolation. Socket sends are replaced by byte counting. The old
  application API and viewer are not compiled. Vertices use signed integer
  positions and signed fixed-point UVs. The adapter uses 16-bit indices,
  one encoder call per supplied frame, and a keyframe every 60 calls; the old
  application's three-frame send throttle is omitted. Thus these figures
  describe its adapted codec, not its out-of-box application FPS. LZ4's 12-byte
  chunk headers are included; WebSocket headers are excluded.
- **raw:** ImGuiQuic canonical serialization without I/P encoding, as a baseline.

For independent decode checks of imgui-ws and netImgui's compressed data, run
`build/comparison/build/codec <algorithm> <moving|dynamic> 120 verify`.
Verification timings must not be mixed with normal results. ImGuiQuic's existing
`tests/browser/draw_ip.mjs` checks exact cross-language reconstruction.

Upstream sources and licenses remain in ignored build directories. The extraction
script does not vendor the upstream implementation into this repository.

## Network measurements

`network.py` uses independently decoding Python receivers for native QUIC and
the actual upstream imgui-ws/incppect/uWebSockets/uSockets stack. Both hosts use
the shared scene plus timestamp and consumed-pointer-sequence markers. It
measures generation-to-marker-decode latency, not GPU presentation. Fonts and
connection setup are excluded. The imgui-ws receiver polls at 30 Hz, disables
WebSocket per-message compression, and subscribes to background and UI lists.
Its variable updates do not guarantee an atomic whole-frame snapshot. The
imgui-ws server is unencrypted; QUIC includes TLS, so server CPU comparisons
include different security costs. netImgui and RemoteImGui network stacks and
viewers are not measured.

Fetch imgui-ws submodules recursively, configure the comparison build with
`-DBUILD_UPSTREAM_WS_HOST=ON`, and build the main project with
`-DIMGUI_QUIC_BUILD_BENCHMARKS=ON`. Prepare the credentials and Python test
environment as described in `../README.md`; additionally install
`websockets==14.2` into that environment. Linux `ip`, `tc` and unprivileged user
and network namespaces are required. All shaping stays inside the new namespace:

```bash
IMGW_BENCH_HOST_NETNS="$(readlink /proc/self/ns/net)" unshare --user --map-root-user --net \
  build/webtransport_venv/bin/python tools/benchmarks/comparison/network.py \
  --seconds 8 --trials 2 --output docs/benchmarks/expanded/network.json
```

`fanout.py` measures 1, 4 and 8 independently ACKing QUIC receivers with the same
namespace command. Only this benchmark host overrides per-address admission to
eight; library admission defaults remain unchanged. It reports server CPU/RSS
and per-client decode throughput, excluding receiver CPU and GPU rendering.

## Quantization and residual-compression experiments

`imgui-quic-q16` uses negotiated production quantized geometry (12-byte vertices)
before the existing I/P encoder. `imgui-quic+lz4` and `imgui-quic-q16+lz4` are
encoder-only experiments: compress the resulting I/P packet with upstream
the project's pinned LZ4, account for a hypothetical four-byte decoded-length
header, and retain the uncompressed packet if smaller. These LZ4 variants are
not implemented in the live wire protocol or browser. Their sizes are estimates
of a possible extension, not actual network traffic. Compression allocations
and size comparison are inside the encode timer. `verify` also checks LZ4
round trips, outside reported performance runs.

For JavaScript decoder costs on identical generated scenes:

```bash
node tools/benchmarks/comparison/decode.mjs
```

This runs the production decoder under Node/V8, including validation and cache
commit, with 30 warmup and 300 measured frames per trial, five trials. Correctness
and error checks are outside the timer. It excludes WebGL and the browser's
compositor, and is not a browser rendering benchmark. The reported cache bytes
sum the nine retained wire baselines, excluding JS object overhead and the
separate expanded frame. Fixture memory is also excluded from this figure.

For live quantized traffic, prefix the transport benchmark command with
`IMGUI_QUIC_TEST_QUANTIZED=1`. The receiver records `quantized_frames` to verify
that negotiation actually selected packed geometry.

## Exact-encoding optimization experiments

The [encoding review](../../../docs/benchmarks/encoding-review/README.md) measures
an equal-block scan and whole-frame XOR/LZ4 against the current exact encoder.
Configure `build/comparison/scalar` with `-DEXPERIMENT_SCALAR_SCAN=ON`, then run
`review.py` to save fresh results to `build/comparison/review.json`. The scalar
reference is generated only in that build directory. `imgui-quic-xor-lz4`
is a codec-only experiment with estimated framing; it is not a live protocol.
The runner compares exact output fixtures and verifies XOR/LZ4 reconstruction
separately from timing.

## Production packed/planar evaluation

`imgui-quic-q1-adaptive-live` measures default integer offsets, u16 UVs and the
complete negotiated planar selector. `imgui-quic-adaptive-live` keeps original
float vertices while enabling planar. The `live` suffix runs the production
encoder, including both candidate costs; it does not open network connections.
`decode.mjs <output> --packed-live` measures the actual JavaScript decoder for
these encoders across six scenes, with error-bound and unchanged-metadata checks
outside timing. `packed_network.py` compares the two negotiated modes over actual
QUIC with an independent Python receiver. Its per-byte decode cost differs from
JavaScript and is retained in the results. Use the isolated namespace command
above; run timings separately. The guide and results are in
[the production report](../../../docs/benchmarks/packed-live/README.md).
The [CPU evaluation](../../../docs/benchmarks/packed-cpu/README.md) compares
identical wire output before and after lane-loop and quantization optimizations;
`cpu.py` accepts two codec binaries and randomizes their timing trials.


`imgui-quic-adaptive` measures the complete production encoder with capability
bit 7 enabled, including codec selection and bounded alternative evaluation.
Run `integrated.py` for five randomized trials per scene. ImGuiQuic compression
uses the core's pinned LZ4 1.10.0. RemoteImGui uses its own bundled LZ4, with
identifier prefixes changed in generated build files to avoid symbol collisions.
Upstream source files are not modified.

## Lossless research candidates

`imgui-quic-plane4` and `imgui-quic-plane20` XOR against the previous canonical
frame, transpose bytes into 4 or 20 lanes, and compress with LZ4. The `-sub`
suffix uses modulo-256 byte subtraction instead of XOR. Transposition spans the
whole canonical frame; these are not yet semantic vertex/attribute codecs.
`verify` independently decompresses, unshuffles and reconstructs original bytes.
Their 13-byte envelope is estimated framing, not a valid negotiated wire type.

Optional `imgui-quic-meshopt4` and `imgui-quic-meshopt20` apply meshoptimizer's
lossless vertex codec to padded, XORed canonical bytes in 4 or 20 byte groups.
They do not reorder triangles or quantize coordinates. This probes the codec's
applicability; it is not an optimized per-list vertex/index integration.

```bash
git clone https://github.com/zeux/meshoptimizer.git build/comparison/upstream/meshoptimizer
git -C build/comparison/upstream/meshoptimizer checkout 9e1f07b159d3cb777f1c67ed31fc11fd117986f4
cmake -S tools/benchmarks/comparison -B build/comparison/build \
  -DCMAKE_BUILD_TYPE=Release -DEXPERIMENT_MESHOPT=ON
cmake --build build/comparison/build -j 6
```

Configure a second build at `build/comparison/byte` with
`-DEXPERIMENT_BYTE_PATCH=ON` to generate a reference without the production
eight-byte changed-range scan. `scan.py` randomizes timings and compares complete
wire fixtures for all seven scenes. Research results and frame-selection design
are in [the expanded report](../../../docs/benchmarks/expanded/README.md).


For the integrated encoder and upstream comparison, the default `run.py` cases
are `imgui-quic-adaptive`, compression-disabled `imgui-quic`, `imgui-ws`,
`netImgui`, `RemoteImGui`, and `raw`. Use `--algorithms` to select other variants.

```bash
python3 tools/benchmarks/comparison/run.py --frames 3000 --trials 7 --output docs/benchmarks/integrated-comparison/results.json
python3 tools/benchmarks/comparison/report.py docs/benchmarks/integrated-comparison/results.json docs/benchmarks/integrated-comparison/README.md
```
