# Expanded remote ImGui evaluation

This report evaluates the earlier source snapshots identified by its raw hashes.
The current production integer/planar codec is evaluated in
[the production comparison](../packed-live/README.md).

Seven shared workloads compare native production encoding with pinned upstream routines. Separate tests compare the actual QUICHE and imgui-ws network stacks, then evaluate additional lossless codec candidates. There is no single winner across payload size, encoding cost, precision and latency.

## Encoder corpus

The corpus uses 5 randomized trials, 900 measured frames and 30 warmup frames, pinned to logical CPU 0. The simulation advances at 30 FPS without pacing. Encoding includes serialization, allocations, codec selection and baseline ownership updates; layout, networking and decoding are excluded. Figures are medians of per-trial means. The working tree is dirty; dependency pins and source hashes are recorded in [raw results](results.json). These measurements precede the eight-byte patch-scan change below.

Scenes use a 1280×720 viewport and a 1000×650 window: stationary text, moving text, changing text, a 4096-point antialiased curve, a 32-row/8-column table, variable triangle counts, and 6000 colored triangles. Vertex counts remain within the older adapters’ 16-bit index limits. Textures and bootstrap are excluded.

| Scene | Encoder | Mean B/frame | Encode mean µs | Encode p95 µs |
|---|---|---:|---:|---:|
| static | imgui-quic-adaptive | 13.0 | 3.20 | 3.22 |
| static | imgui-ws | 12.0 | 20.81 | 21.10 |
| static | netImgui | 160.0 | 3.69 | 3.68 |
| static | RemoteImGui | 546.5 | 30.17 | 32.11 |
| static | raw | 64949.0 | 0.97 | 1.00 |
| moving | imgui-quic-adaptive | 44.0 | 7.70 | 8.31 |
| moving | imgui-ws | 51.8 | 21.47 | 22.58 |
| moving | netImgui | 181.7 | 3.69 | 3.68 |
| moving | RemoteImGui | 683.0 | 30.56 | 32.14 |
| moving | raw | 64949.0 | 0.98 | 1.03 |
| dynamic | imgui-quic-adaptive | 1130.5 | 12.70 | 19.57 |
| dynamic | imgui-ws | 15176.3 | 26.91 | 28.38 |
| dynamic | netImgui | 10721.3 | 4.38 | 4.54 |
| dynamic | RemoteImGui | 1369.7 | 34.19 | 36.34 |
| dynamic | raw | 68301.3 | 1.04 | 1.06 |
| plots | imgui-quic-adaptive | 140156.1 | 426.55 | 559.28 |
| plots | imgui-ws | 374827.6 | 358.32 | 388.97 |
| plots | netImgui | 196291.8 | 55.66 | 71.89 |
| plots | RemoteImGui | 20294.1 | 309.17 | 328.60 |
| plots | raw | 628189.0 | 9.55 | 10.10 |
| tables | imgui-quic-adaptive | 10371.5 | 106.04 | 129.28 |
| tables | imgui-ws | 128609.5 | 131.23 | 138.56 |
| tables | netImgui | 89901.4 | 26.68 | 31.98 |
| tables | RemoteImGui | 7776.9 | 142.08 | 149.85 |
| tables | raw | 277781.0 | 3.70 | 4.16 |
| topology | imgui-quic-adaptive | 29325.4 | 147.54 | 233.90 |
| topology | imgui-ws | 129337.9 | 75.14 | 121.25 |
| topology | netImgui | 58018.9 | 11.23 | 24.56 |
| topology | RemoteImGui | 19788.6 | 98.38 | 158.21 |
| topology | raw | 162283.0 | 6.42 | 15.46 |
| dense | imgui-quic-adaptive | 57476.6 | 553.23 | 694.38 |
| dense | imgui-ws | 576012.0 | 524.21 | 583.05 |
| dense | netImgui | 432152.0 | 121.83 | 163.70 |
| dense | RemoteImGui | 421348.1 | 1773.70 | 1898.78 |
| dense | raw | 1229669.0 | 18.61 | 20.86 |

### Fidelity and adapter limits

ImGuiQuic preserves canonical float32 positions/UVs and u32 indices. netImgui packs positions at approximately 0.25-unit resolution and UVs as u16; RemoteImGui truncates positions to integers and packs fixed-point UVs. Their smaller payloads are not equal-precision comparisons. imgui-ws normalizes positions by subtracting and later restoring an origin in floating point, which can also change fractional bits.

netImgui’s word-oriented changed/unchanged runs and packed contiguous buffers give it the lowest encoding cost among the remoting codecs in this corpus. RemoteImGui’s byte subtraction, LZ4, 12-byte vertices and integer positions favor smooth curves and tables. ImGuiQuic’s motion prediction favors window translation; its XOR/LZ4 path favors changing vertex colors. Raw serialization has no compression work and is a cost floor, not a competing transport.

RemoteImGui’s extracted adapter uses its bundled LZ4, submits every supplied frame and emits a keyframe every 60 calls. Its old application throttle and viewer are not run. imgui-ws’s codec figures omit incppect/WebSocket metadata; netImgui includes command headers. Every encoder here has an immediately available previous frame. These codec figures exclude ACK delay and congestion.

## Actual network stacks

Each profile has two randomized trials of eight measured seconds for each scene and stack. Impaired profiles use a 2 Mbit/s path, configured RTT/jitter/loss in an isolated loopback namespace, with MTU 1500 and offloads disabled. Both hosts generate the shared scene plus timestamp/input markers at 30 FPS. The independent receivers decode and measure generation-to-marker-decode age and pointer-input-to-marker-decode latency. This is not browser/GPU presentation latency. Input percentiles include observed inputs only; the count column exposes missing observations.

QUIC uses TLS, acknowledgements and production frame flow control. imgui-ws runs its actual incppect/uWebSockets/uSockets server without TLS; its receiver subscribes to background/UI variables and polls at 30 Hz instead of the upstream browser’s default 20 Hz. Variable publication is not an atomic whole-frame snapshot. Receiver CPU, font bootstrap and browser rendering are excluded. Wire rates include transport overhead/retransmission counted by the namespace filters. Server CPU measures all process threads; 100% represents one logical core.

| Scene | Profile | Stack | FPS | Wire KiB/s | Marker age p95 ms | Input p95 ms | Inputs seen/sent, both trials | CPU % | RSS MiB |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| moving | local | imgui-quic | 30.00 | 12.40 | 1.42 | 18.01 | 140/140 | 1.19 | 11.79 |
| moving | local | imgui-ws | 29.62 | 9.29 | 31.87 | 58.36 | 140/140 | 0.56 | 7.57 |
| moving | 100ms-1pct | imgui-quic | 29.75 | 11.21 | 56.27 | 125.37 | 139/140 | 1.19 | 11.63 |
| moving | 100ms-1pct | imgui-ws | 27.69 | 9.91 | 85.58 | 153.59 | 137/140 | 0.56 | 7.53 |
| moving | 300ms-3pct | imgui-quic | 18.25 | 7.28 | 160.76 | 383.20 | 105/140 | 0.88 | 11.61 |
| moving | 300ms-3pct | imgui-ws | 14.06 | 7.07 | 491.65 | 727.02 | 104/140 | 0.43 | 7.65 |
| moving | 100ms-5pct | imgui-quic | 28.44 | 10.80 | 60.71 | 149.85 | 139/140 | 1.00 | 11.88 |
| moving | 100ms-5pct | imgui-ws | 18.88 | 8.19 | 186.86 | 301.26 | 125/140 | 0.43 | 7.70 |
| dynamic | local | imgui-quic | 30.00 | 45.59 | 1.24 | 19.97 | 140/140 | 1.06 | 11.76 |
| dynamic | local | imgui-ws | 29.37 | 472.77 | 32.24 | 58.79 | 140/140 | 0.69 | 7.48 |
| dynamic | 100ms-1pct | imgui-quic | 29.49 | 40.39 | 59.61 | 130.60 | 139/140 | 1.00 | 11.73 |
| dynamic | 100ms-1pct | imgui-ws | 14.18 | 230.05 | 685.00 | 724.94 | 127/140 | 0.56 | 7.41 |
| dynamic | 300ms-3pct | imgui-quic | 18.06 | 19.72 | 163.64 | 374.41 | 105/140 | 0.87 | 11.95 |
| dynamic | 300ms-3pct | imgui-ws | 3.31 | 61.97 | 5674.51 | 5688.35 | 12/140 | 0.37 | 7.05 |
| dynamic | 100ms-5pct | imgui-quic | 28.12 | 38.28 | 65.45 | 142.44 | 132/140 | 1.25 | 12.26 |
| dynamic | 100ms-5pct | imgui-ws | 5.62 | 96.56 | 3388.62 | 3477.43 | 51/140 | 0.50 | 7.37 |

[Raw network trials](network.json) and [metadata](network.metadata.json) include display staleness, input observation counts, losses, packet types, source fingerprints, dependency pins and receiver handle times. Handle times are Python implementation costs and should not be ranked as browser decoder speeds. Only two short trials were run: tail results vary considerably, particularly when reliable delivery accumulates a backlog. netImgui and RemoteImGui network stacks remain unmeasured.

## Client fanout

Three local trials per client count, six measured seconds each, use independent QUIC decode/ACK receivers and the dynamic scene. The benchmark alone admits eight clients from the same address. Server CPU/RSS excludes Python receiver processes. All clients observe about 30 FPS. This does not establish many-client behavior under loss.

| Clients | Server CPU % | Server RSS MiB | Lowest per-client FPS across trials |
|---:|---:|---:|---:|
| 1 | 0.83 | 11.67 | 29.95 |
| 4 | 1.33 | 11.84 | 29.95 |
| 8 | 2.00 | 12.44 | 29.81 |

[Raw fanout trials](fanout.json).

## Integrated patch-scan optimization

The production scanner inspects eight bytes together when constructing changed ranges, then trims the equal suffix. Patch boundaries and every wire byte match the prior byte-at-a-time scan. This reduces loop work without introducing a new protocol type or changing precision. Three randomized 900-frame trials per variant compare the complete adaptive encoder, including serialization. The reference is generated from the same source with only this optimization removed.

| Scene | Byte scan µs | Eight-byte scan µs | Mean encode reduction |
|---|---:|---:|---:|
| static | 3.24 | 2.58 | 20.3% |
| moving | 7.76 | 7.22 | 7.0% |
| dynamic | 12.76 | 12.19 | 4.5% |
| plots | 429.70 | 395.21 | 8.0% |
| tables | 106.51 | 102.34 | 3.9% |
| topology | 149.75 | 137.63 | 8.1% |
| dense | 540.44 | 507.85 | 6.0% |

[Raw scan trials](scan.json) also record identical complete wire fixtures for 120 measured frames plus warmup in each scene. The native randomized scanner test compares 2000 grow/shrink/sparse-difference cases against an independent scalar implementation.

## Further lossless codec research

The [research trials](research.json) compare candidates and upstream codecs in one randomized run: three 900-frame trials per case. Candidate timing includes canonical serialization, transform, compression and baseline updates. It excludes a future adaptive selector, browser decode and networking. The estimated 13-byte envelope is not a supported wire packet. These candidates are not enabled in interactive sessions.

| Scene | Encoder / candidate | B/frame | Encode mean µs |
|---|---|---:|---:|
| moving | imgui-quic-adaptive | 44.0 | 7.10 |
| moving | imgui-quic-plane20-sub | 514.4 | 33.42 |
| moving | imgui-quic-meshopt20 | 1893.8 | 78.72 |
| moving | netImgui | 181.7 | 3.85 |
| moving | RemoteImGui | 683.0 | 32.35 |
| dynamic | imgui-quic-adaptive | 1130.5 | 12.06 |
| dynamic | imgui-quic-plane20-sub | 729.2 | 33.40 |
| dynamic | imgui-quic-meshopt20 | 1506.7 | 79.88 |
| dynamic | netImgui | 10721.3 | 4.47 |
| dynamic | RemoteImGui | 1369.7 | 36.09 |
| plots | imgui-quic-adaptive | 140156.1 | 388.73 |
| plots | imgui-quic-plane20-sub | 55698.3 | 422.73 |
| plots | imgui-quic-meshopt20 | 59468.4 | 842.81 |
| plots | netImgui | 196291.8 | 56.84 |
| plots | RemoteImGui | 20294.1 | 328.44 |
| tables | imgui-quic-adaptive | 10371.5 | 102.61 |
| tables | imgui-quic-plane20-sub | 5738.0 | 161.65 |
| tables | imgui-quic-meshopt20 | 11711.0 | 371.28 |
| tables | netImgui | 89901.4 | 26.16 |
| tables | RemoteImGui | 7776.9 | 152.15 |
| topology | imgui-quic-adaptive | 29325.4 | 135.29 |
| topology | imgui-quic-plane20-sub | 16027.1 | 112.85 |
| topology | imgui-quic-meshopt20 | 22454.0 | 241.21 |
| topology | netImgui | 58018.9 | 11.02 |
| topology | RemoteImGui | 19788.6 | 103.76 |
| dense | imgui-quic-adaptive | 57476.6 | 521.93 |
| dense | imgui-quic-plane20-sub | 60762.7 | 870.91 |
| dense | imgui-quic-meshopt20 | 34386.7 | 1606.61 |
| dense | netImgui | 432152.0 | 124.07 |
| dense | RemoteImGui | 421348.1 | 1917.23 |

`plane20-sub` takes a modulo-256 byte difference from the previous canonical frame, transposes it into 20 byte lanes and applies LZ4. `meshopt20` pads whole canonical XOR residuals into 20-byte groups and uses meshoptimizer’s lossless vertex codec. Neither implementation yet separates actual vertex/index/command ranges; they are applicability probes rather than optimized semantic encoders. Verification decompresses, reverses the transform and checks original canonical bytes outside timing.

The [4/20-lane experiments](experiments.json), [XOR/subtraction comparison](residual-experiments.json) and [4/20-byte meshoptimizer experiments](meshopt-experiments.json) use shorter three-trial, 300-frame runs. Their raw results remain separate from the final research comparison. meshoptimizer is pinned to `9e1f07b159d3cb777f1c67ed31fc11fd117986f4` (v1.3); it is an optional benchmark dependency.

### External research and applicability

- [meshoptimizer](https://github.com/zeux/meshoptimizer/tree/9e1f07b159d3cb777f1c67ed31fc11fd117986f4) provides a lossless vertex codec with internal byte deinterleaving and optimized decoders. Its documentation recommends packed, quantized input; codec losslessness does not require applying those lossy preprocessing steps. For ImGui, preserve vertex/triangle order, draw-command offsets and alpha blending. A per-list attribute/index integration is more representative than grouping a whole frame.

- [Bitshuffle](https://github.com/kiyo-masui/bitshuffle) reorganizes typed bits to expose redundancy before LZ4, using SIMD on supported machines. This motivates block-local attribute shuffling rather than repeatedly transposing a whole mixed frame. Its published throughput is not an ImGui benchmark; an ARM and browser/Wasm implementation must be measured separately.

- [Chimp](https://vldb.org/pvldb/vol15/p3058-liakos.pdf) studies lossless floating-point XOR coding and the cost of leading/trailing-zero metadata. It motivates specialized coordinate residuals. ImGui vertex ordering and changing topology differ from a stable time series; coordinate identity must be established before using temporal prediction.

- [Stream VByte](https://arxiv.org/abs/1709.08990) separates integer control bytes from payloads to support SIMD decoding. It is relevant to index deltas and patch offsets/counts. Its paper’s throughput is not a measurement of this project, and extra per-frame metadata can lose on tiny patches.

### Adaptive frame selection

The current encoder already chooses ordinary I/P, motion P and XOR/LZ4 I/P using the acknowledged baseline. A useful extension is a negotiated per-list or per-block codec, while retaining the same frame ID, baseline ID, resource barriers and presentation ACK. Candidate IDs must be distinct from existing compressed-frame IDs; older clients keep the current encoding.

Select from data statistics, not application names or benchmark scenes:

1. Suppress unchanged geometry; use motion P for coherent translations and patch P for small edits.
2. For broad changes, sample changed-byte density, zero residual runs and stable vertex/index/command ranges. Bound sample work and retain a fast path for small packets.
3. Evaluate attribute-shuffled subtraction/LZ4 for coordinate-heavy changes, and specialized color/index coding where samples justify it. Topology changes require per-list ranges so one resized list does not misalign all later residuals.
4. Use a latency/CPU budget and measured network throughput. A decision model can estimate encode cost + decode cost + bytes/available throughput, with backlog/deadline penalties. This is a design hypothesis, not a validated runtime selector. Do not run every full encoder and ignore selection costs in benchmarks.
5. Use hysteresis, periodic bounded re-evaluation and exact fallback. Keep every referenced baseline acknowledged; new codec representations must reconstruct the canonical bytes before cache commit and presentation ACK.

Required acceptance checks for an extension include bounded malformed-input handling, aged ACK bases, grow/shrink, quantized/exact transitions, epoch/recovery, bootstrap single-flight, loss and browser decoding. Whole-frame savings can disappear once fragmentation, retransmission and decoder cost are included. The evidence supports multiple encoding choices; it does not establish a universal best codec.

## Validation and reproduction

See [validation](validation.json) for completed checks and [the benchmark guide](../../../tools/benchmarks/comparison/README.md) for dependencies, pinned checkouts, adapters and network commands. CPU turbo/governor and unrelated desktop workloads were not controlled. All performance measurements are local x86-64; no ARM timing claim is made.

```bash
python3 tools/benchmarks/comparison/run.py --frames 900 --trials 5 --scenes static moving dynamic plots tables topology dense --algorithms imgui-quic-adaptive imgui-ws netImgui RemoteImGui raw --output docs/benchmarks/expanded/results.json
python3 tools/benchmarks/comparison/scan.py
python3 tools/benchmarks/comparison/run.py --frames 900 --trials 3 --scenes moving dynamic plots tables topology dense --algorithms imgui-quic-adaptive imgui-quic-plane20-sub imgui-quic-meshopt20 netImgui RemoteImGui --output docs/benchmarks/expanded/research.json
python3 tools/benchmarks/comparison/expanded_report.py
```

Re-running the first command after the scan optimization produces the newer encoder, not the archived pre-optimization source hashes. Preserve original reports when evaluating later changes.

## Upstream references

- [imgui-ws `5e51bbf43a50`](https://github.com/ggerganov/imgui-ws/tree/5e51bbf43a50d5f2c53124ab33f8740fa3f99b70)
- [netImgui `bf471d8458d1`](https://github.com/sammyfreg/netImgui/tree/bf471d8458d1841bfea94fe07b6b89f630a8ba7c)
- [remoteimgui `fc6b8de10bdb`](https://github.com/JordiRos/remoteimgui/tree/fc6b8de10bdb7f3e38b53a1a8357f8df391ebc07)
