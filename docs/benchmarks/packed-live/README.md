# Integer geometry and planar compression

The browser defaults to integer position offsets, u16 UVs and negotiated planar subtraction/LZ4. Small patches retain the fast path. Large changing attributes can use lane-transposed residuals; stable attribute samples skip the second compressor. These formats run in the native server and production browser.

Nearest integer offsets relative to each list’s original float origin bound coordinate error to half a logical unit. UV error is approximately 1/131070 plus float reconstruction rounding. Colors, indices, clips, textures, offsets and input coordinates remain unchanged. Quarter, fine and exact modes are selectable. Older clients/servers negotiate their supported formats.

## Same-run encoding comparison

Five randomized trials per case use 900 measured frames and 30 warmup frames, logical CPU 0, the same pinned Dear ImGui and Release compiler. Timings include serialization, quantization, allocation, ordinary/planar candidate evaluation, sampling, selection and baseline updates; they exclude layout/network/decoding. Figures are medians of per-trial statistics. Source hashes identify the measured source in [raw results](results.json).

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
| static | 2.61 | 2.58 | 9.50 | 30.69 | 3.65 |
| moving | 7.06 | 7.06 | 9.66 | 30.91 | 3.66 |
| dynamic | 11.31 | 11.19 | 19.03 | 34.50 | 4.37 |
| plots | 387.29 | 675.12 | 332.10 | 296.42 | 55.99 |
| tables | 101.22 | 188.70 | 155.73 | 144.50 | 25.62 |
| topology | 135.17 | 207.32 | 154.98 | 98.44 | 10.90 |
| dense | 504.06 | 497.86 | 356.44 | 1754.05 | 120.63 |

The default has lower mean payload than netImgui and RemoteImGui in all seven measured scenes. imgui-ws is one byte smaller for forced static encoder calls (12 versus 13 bytes); production suppresses identical geometry apart from heartbeat/resource changes. Full imgui-ws results, first-frame/p95 sizes and timing variation remain in the raw file. This is a payload result, not a universal performance ranking.

Encoding still costs more than netImgui. Planar evaluation increases curve/table/topology CPU work; packing unchanged/moving text costs more than the exact path. Attribute sampling prevents the dense-color workload’s rejected second compression, but can miss size savings. Compression preserves selected packed bytes exactly; packing intentionally changes floats.

netImgui uses approximately 0.25-unit position precision and u16 UVs. RemoteImGui truncates positions to integers, packs fixed-point UVs and uses an older command schema. ImGuiQuic rounds relative offsets to nearest integers while retaining float list origins, so integer geometry is not identical between implementations. Upstream adapter/header differences, throttle adaptations and omitted texture bootstrap are documented in [the guide](../../../tools/benchmarks/comparison/README.md). All codecs assume immediate previous-frame availability here.

## Production JavaScript decoding

Five Node/V8 trials use 30 warmup and 120 measured frames from the actual C++ encoder. The timer includes decoding, LZ4, unshuffle/addition, quantized expansion, validation and cache commit. Error-bound and unchanged-metadata checks run outside timing. WebGL/compositor are excluded. Case order is fixed; GC/desktop activity are uncontrolled. These are not browser presentation timings.

| Scene | Exact XOR/LZ4 mean/p95 µs | Exact + planar mean/p95 µs | Integer + planar mean/p95 µs |
|---|---:|---:|---:|
| moving | 89/103 | 82/92 | 824/917 |
| dynamic | 439/514 | 430/501 | 870/1137 |
| plots | 4702/4971 | 1689/1879 | 4772/5053 |
| tables | 1500/1859 | 650/726 | 2806/2912 |
| topology | 1779/3095 | 724/1091 | 1691/2654 |
| dense | 7387/7821 | 7297/7789 | 13802/14506 |

[Raw decode trials](decode.json) include cache/expanded bytes and position/UV error. Nine packed or float baselines remain bounded separately from the expanded frame.  Device-specific browser and ARM timings remain unmeasured.

## Actual native QUIC comparison

Two randomized six-second trials per mode/scene/profile use the same native host and independent Python receivers. `exact-xor` requests exact geometry with planar disabled; `integer-planar` requests integer packing and planar. All sixteen cases passed. The impaired profile uses 100 ms configured RTT, 10 ms jitter, 5% loss and 2 Mbit/s in an isolated loopback namespace with MTU 1500/offloads disabled. Setup/font bootstrap is excluded.

| Scene | Profile | Mode | FPS | Wire KiB/s | Frame age p95 ms | Input p95 ms | Inputs observed/sent, both trials |
|---|---|---|---:|---:|---:|---:|---:|
| plots | local | exact-xor | 15.00 | 2266.28 | 38.85 | 67.22 | 100/100 |
| plots | local | integer-planar | 12.44 | 130.43 | 158.83 | 161.99 | 98/100 |
| plots | 100ms-5pct | exact-xor | 0.75 | 112.06 | 1490.54 | 1536.78 | 7/100 |
| plots | 100ms-5pct | integer-planar | 4.58 | 58.46 | 512.90 | 517.72 | 42/100 |
| tables | local | exact-xor | 30.06 | 384.64 | 13.75 | 38.15 | 100/100 |
| tables | local | integer-planar | 28.98 | 144.97 | 138.18 | 158.71 | 100/100 |
| tables | 100ms-5pct | exact-xor | 7.50 | 81.34 | 315.89 | 438.80 | 65/100 |
| tables | 100ms-5pct | integer-planar | 16.55 | 77.50 | 322.36 | 424.32 | 77/100 |

Weak-network freshness and received FPS improve. Local curves have lower FPS/higher age with the Python planar receiver: per-byte unshuffle/expand costs are retained in the raw trials. This receiver bottleneck is retained in the report. The separate production JavaScript decoder measurement does not establish browser end-to-end FPS.

Wire rates include QUIC overhead/retransmissions; delivering more frames can increase throughput despite smaller frames. Age ends at decoded timestamp/consumed-pointer markers, not GPU scanout. Input percentiles include observed inputs only. Large P frames still use reliable delivery and can stall under loss. Independent cancelable frame streams are not implemented. Short two-trial tail estimates vary. [Raw network trials](network.json) include server CPU/RSS, packet types, losses and receiver costs.

## Validation

[Validation records](validation.json) cover seven CTests, frontend suites, 450 quantized error-bound fixtures, 240 planar cross-language fixtures, 12/20 lanes, aged bases, precision/size transitions, malformed LZ4/stride rejection, presentation-before-ACK and recovery. Native QUIC checks cover integer, quarter and fine modes. SIMD/scalar equivalence, sanitizers and forced-scalar builds are recorded in [quantization validation](../sse2-integration/README.md). This does not establish every DPI/font or ARM performance.

## Reproduce

Use the pins and build setup in [the guide](../../../tools/benchmarks/comparison/README.md).

```bash
python3 tools/benchmarks/comparison/run.py --frames 900 --trials 5 --scenes static moving dynamic plots tables topology dense --algorithms imgui-quic-adaptive imgui-quic-adaptive-live imgui-quic-q1-adaptive-live imgui-ws netImgui RemoteImGui --output docs/benchmarks/packed-live/results.json
node tools/benchmarks/comparison/decode.mjs docs/benchmarks/packed-live/decode.json
IMGW_BENCH_HOST_NETNS="$(readlink /proc/self/ns/net)" unshare --user --map-root-user --net build/webtransport_venv/bin/python tools/benchmarks/comparison/packed_network.py --seconds 6 --trials 2 --output docs/benchmarks/packed-live/network.json
python3 tools/benchmarks/comparison/packed_report.py
```

All timings are x86-64. Compiler, source/dependency hashes and scenes are in raw encoding metadata. CPU turbo/governor and unrelated desktop work were not controlled; assess trial variability rather than small timing differences.
