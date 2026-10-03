# ImGuiQuic and upstream remoting encoders

This comparison includes all variants in the same randomized run. It measures actual upstream encoding routines on the same Dear ImGui workload, not published numbers or separate demo applications.

Each case has 7 trials, 30 warmup frames and 3000 measured frames per trial. All encoders use the same Dear ImGui commit `b61e56346a92cfcaf1f43a545ca37b0b32239654`, compiler `c++ (GCC) 16.2.1 20260810`, Release optimization and logical CPU 0. The workload is 1280×720 with a 1000×650 window and 30 text rows. The runner randomizes case order with seed 42.

The measured working tree contains uncommitted integration changes; raw metadata includes source hashes and dependency pins. The Git commit alone does not identify this encoder.

Rows use medians of per-trial statistics. Encode time includes serialization, encoding, related allocations and baseline updates, but not ImGui layout, socket I/O, client decoding or display. The mean-time range shows variation across trials. The 30 FPS rate is calculated draw-payload throughput; it is not measured TCP/UDP traffic.

## Moving scene

| Encoder | First UI frame B | Mean payload B/frame | Encode mean µs | Encode p95 µs | Trial mean range µs | Payload KiB/s at 30 FPS |
|---|---:|---:|---:|---:|---:|---:|
| imgui-quic-adaptive | 33753 | 44.0 | 7.51 | 8.04 | 7.50–7.65 | 1.29 |
| imgui-quic | 64962 | 44.0 | 7.51 | 8.04 | 7.48–7.54 | 1.29 |
| imgui-ws | 57408 | 51.8 | 21.04 | 22.11 | 20.80–23.42 | 1.52 |
| netImgui | 37616 | 187.1 | 3.60 | 3.60 | 3.54–3.62 | 5.48 |
| RemoteImGui | 22802 | 680.5 | 25.98 | 27.14 | 25.93–26.61 | 19.94 |
| raw | 64949 | 64949.0 | 0.94 | 0.96 | 0.94–1.00 | 1902.80 |

## Dynamic scene

| Encoder | First UI frame B | Mean payload B/frame | Encode mean µs | Encode p95 µs | Trial mean range µs | Payload KiB/s at 30 FPS |
|---|---:|---:|---:|---:|---:|---:|
| imgui-quic-adaptive | 33181 | 1154.8 | 16.18 | 20.76 | 16.05–16.39 | 33.83 |
| imgui-quic | 64962 | 8924.9 | 17.97 | 20.23 | 17.91–18.71 | 261.47 |
| imgui-ws | 57408 | 15107.5 | 26.78 | 27.86 | 26.73–27.27 | 442.60 |
| netImgui | 37616 | 10701.3 | 4.34 | 4.51 | 4.34–4.42 | 313.52 |
| RemoteImGui | 22763 | 1361.1 | 29.71 | 30.78 | 29.31–29.73 | 39.87 |
| raw | 64949 | 70322.7 | 0.98 | 0.99 | 0.97–1.00 | 2060.23 |

## Static scene

| Encoder | First UI frame B | Mean payload B/frame | Encode mean µs | Encode p95 µs | Trial mean range µs | Payload KiB/s at 30 FPS |
|---|---:|---:|---:|---:|---:|---:|
| imgui-quic-adaptive | 33181 | 13.0 | 3.09 | 3.12 | 3.06–3.13 | 0.38 |
| imgui-quic | 64962 | 13.0 | 3.12 | 3.14 | 3.09–3.13 | 0.38 |
| imgui-ws | 57408 | 12.0 | 20.24 | 20.20 | 20.22–20.56 | 0.35 |
| netImgui | 37616 | 167.6 | 3.60 | 3.60 | 3.58–3.63 | 4.91 |
| RemoteImGui | 22763 | 546.5 | 25.58 | 25.56 | 25.20–25.73 | 16.01 |
| raw | 64949 | 64949.0 | 0.96 | 0.98 | 0.93–0.97 | 1902.80 |

## Interpretation and limits

- `imgui-quic-adaptive` is the integrated, default-negotiated lossless encoder: block skipping, motion prediction and bounded XOR/LZ4 selection. All selection costs are timed. `imgui-quic` disables compression but retains the optimized exact scan.
- ImGuiQuic compression uses LZ4 `ebb370ca83af193212df4dcbadcc5d87bc0de2f0`; RemoteImGui retains its own bundled LZ4. Its generated dependency changes symbol prefixes only to avoid linking collisions.
- imgui-ws uses its upstream default vertex-offset XOR/RLE encoder, adapted only for the Dear ImGui texture-ID accessor. Its diff size excludes incppect/WebSocket framing and uses 16-bit indices/32-bit texture IDs.
- netImgui uses the upstream conversion/compression functions and includes its command header. It quantizes positions to approximately 0.25-unit steps and UVs to u16, so it does not preserve the same geometry precision as the exact codec.
- RemoteImGui uses extracted upstream packing/difference/LZ4 code with counted socket writes, integer positions, fixed-point UVs and its older command schema. Its bundled LZ4 and chunk headers are included. The adapter submits each frame, without the old three-frame throttle, and emits a keyframe every 60 calls. Its old viewer and networking are not run.
- `raw` is canonical uncompressed serialization, not a remoting repository.
- All codecs assume availability of the previous frame. ACK delay, retransmission and backpressure are excluded. Static scenes force repeated encoder calls and do not model application-level suppression. First-frame bytes omit font textures and connection setup.
- These workloads test small text-heavy UIs, not large plots, images, many clients or every Dear ImGui feature. CPU turbo/governor and unrelated desktop activity are not controlled.

## Sources

- [imgui-ws `5e51bbf43a50`](https://github.com/ggerganov/imgui-ws/tree/5e51bbf43a50d5f2c53124ab33f8740fa3f99b70)
- [netImgui `bf471d8458d1`](https://github.com/sammyfreg/netImgui/tree/bf471d8458d1841bfea94fe07b6b89f630a8ba7c)
- [remoteimgui `fc6b8de10bdb`](https://github.com/JordiRos/remoteimgui/tree/fc6b8de10bdb7f3e38b53a1a8357f8df391ebc07)

## Reproduce

```bash
python3 tools/benchmarks/comparison/run.py --frames 3000 --trials 7 --output docs/benchmarks/integrated-comparison/results.json
python3 tools/benchmarks/comparison/report.py docs/benchmarks/integrated-comparison/results.json docs/benchmarks/integrated-comparison/README.md
```

- [Raw trials](results.json)
- [Validation results](validation.json): imgui-ws and netImgui round trips passed for all three scenes (1500 frames each), outside timing. RemoteImGui’s isolated LZ4 source matches upstream except symbol prefixes; its complete viewer round trip remains untested.
- [Build and adapter details](../../../tools/benchmarks/comparison/README.md)
- [ImGuiQuic decoder and actual network measurements](../integrated/README.md)

Only ImGuiQuic has actual network and JavaScript decoder measurements in this experiment. Competing end-to-end traffic, CPU/RSS and input-to-display latency remain unmeasured; encoder results must not be presented as a full-product performance ranking.
