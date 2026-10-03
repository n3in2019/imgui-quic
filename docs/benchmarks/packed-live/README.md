# Integer geometry and planar compression

The browser defaults to integer position offsets, u16 UVs and negotiated planar subtraction/LZ4. Small patches retain the fast path. Large changing attributes can use lane-transposed residuals; stable attribute samples skip the second compressor. These formats run in the native server and production browser.

Nearest integer offsets relative to each list’s original float origin bound coordinate error to half a logical unit. UV error is approximately 1/131070 plus float reconstruction rounding. Colors, indices, clips, textures, offsets and input coordinates remain unchanged. Quarter, fine and exact modes are selectable. Older clients/servers negotiate their supported formats.

## Same-run encoding comparison

Five randomized trials per case use 900 measured frames and 30 warmup frames, logical CPU 0, the same pinned Dear ImGui and Release compiler. Timings include serialization, quantization, allocation, ordinary/planar candidate evaluation, sampling, selection and baseline updates; they exclude layout/network/decoding. Figures are medians of per-trial statistics. Source hashes identify the dirty working tree in [raw results](results.json).

`imgui-quic-adaptive` disables planar/quantization. `imgui-quic-adaptive-live` enables planar with float geometry. `imgui-quic-q1-adaptive-live` measures the integer/planar browser default. Production selection uses frame bytes, a 4096-byte threshold and up to 64 position/UV samples per list, never benchmark-scene names.

### B/frame

| Scene | Exact XOR/LZ4 | Exact + planar | Integer + planar | RemoteImGui | netImgui |
|---|---:|---:|---:|---:|---:|
| static | 13.00 | 13.00 | 13.00 | 546.55 | 160.00 |
| moving | 43.99 | 43.99 | 35.89 | 682.97 | 181.65 |
| dynamic | 1130.47 | 1100.21 | 939.05 | 1369.65 | 10721.26 |
| plots | 140156.13 | 55699.34 | 8009.67 | 20294.12 | 196291.77 |
| tables | 10371.55 | 5739.02 | 4703.48 | 7776.91 | 89901.36 |
| topology | 29325.42 | 16028.05 | 13516.42 | 19788.63 | 58018.93 |
| dense | 57476.56 | 57476.56 | 57104.30 | 421348.10 | 432152.00 |

### Encode mean µs

| Scene | Exact XOR/LZ4 | Exact + planar | Integer + planar | RemoteImGui | netImgui |
|---|---:|---:|---:|---:|---:|
| static | 2.65 | 2.70 | 19.95 | 32.44 | 3.81 |
| moving | 7.18 | 7.16 | 19.99 | 33.70 | 3.74 |
| dynamic | 11.41 | 12.35 | 30.55 | 35.90 | 4.43 |
| plots | 388.71 | 774.13 | 507.45 | 332.21 | 58.06 |
| tables | 106.82 | 243.53 | 241.92 | 154.45 | 28.05 |
| topology | 135.16 | 241.60 | 201.60 | 105.57 | 11.33 |
| dense | 531.38 | 530.46 | 518.87 | 1869.05 | 128.66 |

The default has lower mean payload than netImgui and RemoteImGui in all seven measured scenes. imgui-ws is one byte smaller for forced static encoder calls (12 versus 13 bytes); production suppresses identical geometry apart from heartbeat/resource changes. Full imgui-ws results, first-frame/p95 sizes and timing variation remain in the raw file. This is a payload result, not a universal performance ranking.

Encoding still costs more than netImgui. Planar evaluation increases curve/table/topology CPU work; packing unchanged/moving text costs more than the exact path. Attribute sampling prevents the dense-color workload’s rejected second compression, but can miss size savings. Compression preserves selected packed bytes exactly; packing intentionally changes floats.

netImgui uses approximately 0.25-unit position precision and u16 UVs. RemoteImGui truncates positions to integers, packs fixed-point UVs and uses an older command schema. ImGuiQuic rounds relative offsets to nearest integers while retaining float list origins, so integer geometry is not identical between implementations. Upstream adapter/header differences, throttle adaptations and omitted texture bootstrap are documented in [the guide](../../../tools/benchmarks/comparison/README.md). All codecs assume immediate previous-frame availability here.

## Production JavaScript decoding

Five Node/V8 trials use 30 warmup and 120 measured frames from the actual C++ encoder. The timer includes decoding, LZ4, unshuffle/addition, quantized expansion, validation and cache commit. Error-bound and unchanged-metadata checks run outside timing. WebGL/compositor are excluded. Case order is fixed; GC/desktop activity are uncontrolled. These are not browser presentation timings.

| Scene | Exact XOR/LZ4 mean/p95 µs | Exact + planar mean/p95 µs | Integer + planar mean/p95 µs |
|---|---:|---:|---:|
| moving | 66/109 | 62/81 | 559/631 |
| dynamic | 300/346 | 290/335 | 765/832 |
| plots | 4926/5217 | 1713/1840 | 4863/5135 |
| tables | 1557/1965 | 707/774 | 2987/3180 |
| topology | 1408/2110 | 733/1103 | 1714/2689 |
| dense | 7504/8154 | 11210/11750 | 13965/14481 |

[Raw decode trials](decode.json) include cache/expanded bytes and position/UV error. Nine packed or float baselines remain bounded separately from the expanded frame. The largest mean default decode is about 14 ms. Device-specific browser and ARM timings remain unmeasured.

## Actual native QUIC comparison

Two randomized six-second trials per mode/scene/profile use the same native host and independent Python receivers. `exact-xor` requests exact geometry with planar disabled; `integer-planar` requests integer packing and planar. All sixteen cases passed. The impaired profile uses 100 ms configured RTT, 10 ms jitter, 5% loss and 2 Mbit/s in an isolated loopback namespace with MTU 1500/offloads disabled. Setup/font bootstrap is excluded.

| Scene | Profile | Mode | FPS | Wire KiB/s | Frame age p95 ms | Input p95 ms | Inputs observed/sent, both trials |
|---|---|---|---:|---:|---:|---:|---:|
| plots | local | exact-xor | 29.74 | 4397.04 | 20.61 | 39.38 | 100/100 |
| plots | local | integer-planar | 24.92 | 231.86 | 81.08 | 86.10 | 100/100 |
| plots | 100ms-5pct | exact-xor | 0.67 | 111.52 | 1397.64 | 1411.96 | 6/100 |
| plots | 100ms-5pct | integer-planar | 5.92 | 72.78 | 389.70 | 494.94 | 54/100 |
| tables | local | exact-xor | 29.99 | 383.43 | 4.97 | 26.06 | 100/100 |
| tables | local | integer-planar | 30.00 | 168.28 | 15.86 | 31.55 | 100/100 |
| tables | 100ms-5pct | exact-xor | 8.17 | 83.18 | 296.79 | 372.11 | 67/100 |
| tables | 100ms-5pct | integer-planar | 18.82 | 98.48 | 249.32 | 352.96 | 82/100 |

Weak-network freshness and received FPS improve. Local curves have lower FPS/higher age with the Python planar receiver: per-byte unshuffle/expand loops take roughly 40 ms per frame. This receiver bottleneck is retained in the report. The separate production JavaScript curve decoder measures about 5 ms; that difference does not establish browser end-to-end FPS.

Wire rates include QUIC overhead/retransmissions; delivering more frames can increase throughput despite smaller frames. Age ends at decoded timestamp/consumed-pointer markers, not GPU scanout. Input percentiles include observed inputs only. Large P frames still use reliable delivery and can stall under loss. Independent cancelable frame streams are not implemented. Short two-trial tail estimates vary. [Raw network trials](network.json) include server CPU/RSS, packet types, losses and receiver costs.

## Validation

[Validation records](validation.json) cover six CTests, frontend suites, 450 quantized error-bound fixtures, 240 planar cross-language fixtures, 12/20 lanes, aged bases, precision/size transitions, malformed LZ4/stride rejection, presentation-before-ACK and recovery. Native QUIC checks cover integer, quarter, fine and legacy exact modes. The demo rendered in the in-app browser without console warnings/errors. This does not establish every DPI/font or ARM performance.

## Reproduce

Use the pins and build setup in [the guide](../../../tools/benchmarks/comparison/README.md).

```bash
python3 tools/benchmarks/comparison/run.py --frames 900 --trials 5 --scenes static moving dynamic plots tables topology dense --algorithms imgui-quic-adaptive imgui-quic-adaptive-live imgui-quic-q1-adaptive-live imgui-ws netImgui RemoteImGui --output docs/benchmarks/packed-live/results.json
node tools/benchmarks/comparison/decode.mjs docs/benchmarks/packed-live/decode.json --packed-live
IMGW_BENCH_HOST_NETNS="$(readlink /proc/self/ns/net)" unshare --user --map-root-user --net build/webtransport_venv/bin/python tools/benchmarks/comparison/packed_network.py --seconds 6 --trials 2 --output docs/benchmarks/packed-live/network.json
python3 tools/benchmarks/comparison/packed_report.py
```

All timings are x86-64. Compiler, source/dependency hashes and scenes are in raw encoding metadata. CPU turbo/governor and unrelated desktop work were not controlled; assess trial variability rather than small timing differences.
