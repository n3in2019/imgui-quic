# Remote ImGui encoder benchmark

ImGuiQuic produces the smallest moving-window payload in these workloads, while netImgui uses the least encoder time. RemoteImGui’s quantization plus LZ4 produces the smallest dynamic-text payload. These are encoder measurements, not an end-to-end ranking of remoting products.

## Method

AMD Ryzen AI 9 HX 470, Linux x86-64, GCC 16.2.1, Release (`-O3 -DNDEBUG`), CPU affinity 0. All codecs use Dear ImGui at `b61e56346a92cfcaf1f43a545ca37b0b32239654`. Each of five randomized trials has 30 warmup and 900 measured frames; the simulation step is 30 FPS. Measurements are not wall-clock paced. The viewport is 1280×720, with a 1000×650 window and 30 text rows. CPU governor, turbo and unrelated desktop activity are not controlled.

Numbers below are medians of trial means. Times cover draw serialization and encoding, including in-block allocations and baseline updates. Rendering, decoding, input, font textures, connection setup and network framing are excluded. No ACK delay is modeled. See the [harness](../../../tools/benchmarks/comparison/README.md) and [raw trials](codec.json).

## Moving window

| Encoder | Bytes/frame | Payload at 30 FPS (KiB/s) | Encode mean (µs/frame) | Encode p95 (µs/frame) |
|---|---:|---:|---:|---:|
| imgui-quic | 44.0 | 1.29 | 43.70 | 45.86 |
| imgui-ws | 51.8 | 1.52 | 20.87 | 22.21 |
| netImgui | 181.7 | 5.32 | 3.61 | 3.62 |
| RemoteImGui | 683.0 | 20.01 | 29.56 | 31.04 |
| raw | 64949.0 | 1902.80 | 0.92 | 0.96 |

The payload-rate column is arithmetic, not a packet capture. ImGuiQuic uses about 15% fewer draw-payload bytes than imgui-ws in this scene, at about twice the encoding time. Its roughly 44 µs/frame corresponds to about 0.13% of one CPU at 30 FPS for this stage alone.

## Dynamic text

| Encoder | Bytes/frame | Payload at 30 FPS (KiB/s) | Encode mean (µs/frame) | Encode p95 (µs/frame) |
|---|---:|---:|---:|---:|
| imgui-quic | 9003.4 | 263.77 | 43.86 | 46.06 |
| imgui-ws | 15176.3 | 444.62 | 26.37 | 27.69 |
| netImgui | 10721.3 | 314.10 | 4.30 | 4.47 |
| RemoteImGui | 1369.7 | 40.13 | 32.77 | 34.64 |
| raw | 68301.3 | 2001.01 | 0.99 | 0.99 |

ImGuiQuic uses about 41% fewer payload bytes than imgui-ws and 16% fewer than netImgui here. RemoteImGui is much smaller, but also changes geometry precision and command representation. This experiment does not isolate how much of its gain comes from LZ4 versus quantization.

## Static scene

| Encoder | Encoded bytes/frame | Encode mean (µs/frame) |
|---|---:|---:|
| imgui-quic | 13.0 | 39.18 |
| imgui-ws | 12.0 | 20.43 |
| netImgui | 160.0 | 3.62 |
| RemoteImGui | 546.5 | 28.14 |
| raw | 64949.0 | 0.92 |

These calls deliberately encode every frame. Real unchanged-frame suppression and polling/heartbeat behavior are outside the test, so the table is not idle bandwidth or idle CPU. RemoteImGui includes its periodic keyframes.

## Precision and adapter boundaries

- ImGuiQuic preserves original position/UV floats and canonical geometry bytes. It uses u32 indices and u64 texture IDs.
- imgui-ws uses float32 vertices normalized to the first vertex, u16 indices and u32 texture IDs. Its returned diff size excludes incppect/WebSocket metadata. The adapter only replaces the removed `TextureId` member with `GetTexID()`.
- netImgui converts vertices to 12-byte records: u16 positions over a 16384-unit range (approximately 0.25-unit steps) and u16 UVs. Its compressed representation is exact relative to those quantized records, not to the input floats.
- RemoteImGui uses integer positions and fixed-point UVs, and its older draw commands omit modern texture/offset semantics. The adapter extracts upstream serializer/difference/LZ4 code, replaces socket writes with counting, omits the three-frame send throttle, and sets a keyframe every 60 encoder calls. Its bundled LZ4 and 12-byte chunk headers are used. The original application/viewer is not run.
- imgui-ws and netImgui delta reconstruction was checked separately on moving/dynamic workloads; those verification runs are excluded from timings. RemoteImGui results measure the extracted encoder, without a browser reconstruction test.

## Source revisions

- [imgui-ws `5e51bbf43a50`](https://github.com/ggerganov/imgui-ws/tree/5e51bbf43a50d5f2c53124ab33f8740fa3f99b70)
- [netImgui `bf471d8458d1`](https://github.com/sammyfreg/netImgui/tree/bf471d8458d1841bfea94fe07b6b89f630a8ba7c)
- [remoteimgui `fc6b8de10bdb`](https://github.com/JordiRos/remoteimgui/tree/fc6b8de10bdb7f3e38b53a1a8357f8df391ebc07)
- ImGuiQuic base: `1f73a5211c92855f8069e358ce9e150b5e3d2064`; benchmark additions and the benchmark-host initialization fix are in the working tree.

## Decision

Keep the existing motion codec for exact moving-window geometry. For weak-network dynamic text, prioritize testing lossless compression of residual patches before accepting geometry quantization. Encoder CPU is higher than netImgui but only tens of microseconds per frame in this workload. Full browser/native-client comparisons are still needed before choosing a transport based on end-to-end latency.

## ImGuiQuic network measurements

The native QUICHE process was measured independently with the Python aioquic receiver, at 30 target FPS. Each profile/scene has two 8-second samples after the first frame and one second of warmup. The benchmark uses isolated loopback netem with offloads disabled. Impaired profiles share a 2 Mbit/s bidirectional shaping budget, with jitter and per-trial seeds. Local is unshaped. Timed codec runs completed before this final network run.

This is not browser input-to-photon latency. Input latency ends at receiving/decoding the frame reflecting a pointer sequence; unobserved/coalesced pointer updates are excluded from that latency distribution. The observation counts therefore accompany latency. The timestamp-marker scene differs from the codec workload. [Raw network trials](network.json).

| Profile | Scene | FPS range | Input p95 range (ms) | Observed/sent inputs, trials | Wire KiB/s range |
|---|---|---:|---:|---|---:|
| local | moving | 30.00–30.00 | 9.79–14.84 | 70/70, 70/70 | 11.23–11.33 |
| local | dynamic | 29.99–30.00 | 17.24–19.24 | 70/70, 70/70 | 317.11–317.59 |
| 100ms-1pct | moving | 29.74–29.87 | 126.25–138.72 | 68/70, 68/70 | 9.96–10.04 |
| 100ms-1pct | dynamic | 10.75–10.87 | 249.77–297.48 | 52/70, 57/70 | 122.30–122.51 |
| 300ms-3pct | moving | 17.62–21.12 | 373.16–378.11 | 62/70, 49/70 | 6.80–7.54 |
| 300ms-3pct | dynamic | 2.87–3.87 | 587.91–988.30 | 24/70, 19/70 | 34.41–44.05 |
| 100ms-5pct | moving | 28.62–29.25 | 142.00–144.97 | 66/70, 64/70 | 9.25–9.61 |
| 100ms-5pct | dynamic | 6.87–8.00 | 378.30–493.44 | 50/70, 41/70 | 76.13–89.06 |

Native process CPU across cases was 1.25–3.00% of one logical CPU; resident memory was 11.27–12.74 MiB. These figures exclude the receiver and browser GPU. High-latency dynamic-text delivery was poor: 2.87–3.87 FPS at 300 ms/3% loss, with only 19–24 of 70 pointer sequences observed. One trial had a sampled displayed-frame age p95 of 6544 ms, including carry-over of a stale frame from warmup. Delivered-frame latency alone hides this stall.

Two short samples are exploratory, especially for lossy profiles. No competing transport was measured end-to-end; these data cannot establish that QUIC is faster than WebSocket or netImgui TCP.
