# ImGuiQuic and upstream remoting encoders

This comparison includes all variants in the same randomized run. It measures actual upstream encoding routines on the same Dear ImGui workload, not published numbers or separate demo applications.

Each case has 7 trials, 30 warmup frames and 1500 measured frames per trial. All encoders use the same Dear ImGui commit `b61e56346a92cfcaf1f43a545ca37b0b32239654`, compiler `c++ (GCC) 16.2.1 20260810`, Release optimization and logical CPU 0. The workload is 1280×720 with a 1000×650 window and 30 text rows. The runner randomizes case order with seed 42.

Rows use medians of per-trial statistics. Encode time includes serialization, encoding, related allocations and baseline updates, but not ImGui layout, socket I/O, client decoding or display. The mean-time range shows variation across trials. The 30 FPS rate is calculated draw-payload throughput; it is not measured TCP/UDP traffic.

## Moving scene

| Encoder | First UI frame B | Mean payload B/frame | Encode mean µs | Encode p95 µs | Trial mean range µs | Payload KiB/s at 30 FPS |
|---|---:|---:|---:|---:|---:|---:|
| imgui-quic | 64962 | 44.1 | 44.53 | 46.20 | 44.28–44.81 | 1.29 |
| imgui-quic-q16 | 45050 | 36.0 | 77.38 | 80.58 | 74.68–78.32 | 1.05 |
| imgui-quic+lz4 | 33757 | 43.5 | 44.52 | 45.30 | 44.42–44.67 | 1.27 |
| imgui-quic-q16+lz4 | 26802 | 36.0 | 76.95 | 80.18 | 73.45–78.37 | 1.05 |
| imgui-ws | 57408 | 51.8 | 21.27 | 22.60 | 21.20–21.44 | 1.52 |
| netImgui | 37616 | 181.8 | 3.67 | 3.67 | 3.66–3.72 | 5.32 |
| RemoteImGui | 22802 | 682.5 | 27.61 | 28.95 | 26.81–27.84 | 19.99 |
| raw | 64949 | 64949.0 | 0.95 | 0.98 | 0.94–0.98 | 1902.80 |

## Dynamic scene

| Encoder | First UI frame B | Mean payload B/frame | Encode mean µs | Encode p95 µs | Trial mean range µs | Payload KiB/s at 30 FPS |
|---|---:|---:|---:|---:|---:|---:|
| imgui-quic | 64962 | 8959.2 | 46.35 | 48.13 | 46.05–47.03 | 262.48 |
| imgui-quic-q16 | 45050 | 8510.2 | 90.02 | 95.25 | 85.52–91.06 | 249.32 |
| imgui-quic+lz4 | 33183 | 4961.9 | 55.17 | 58.09 | 55.11–55.29 | 145.37 |
| imgui-quic-q16+lz4 | 26801 | 4568.2 | 96.30 | 101.26 | 92.90–97.73 | 133.83 |
| imgui-ws | 57408 | 15138.5 | 27.22 | 28.52 | 27.09–28.55 | 443.51 |
| netImgui | 37616 | 10708.9 | 4.45 | 4.65 | 4.44–4.46 | 313.74 |
| RemoteImGui | 22763 | 1364.2 | 31.25 | 32.52 | 30.35–31.63 | 39.97 |
| raw | 64949 | 69456.4 | 1.00 | 1.02 | 0.99–1.01 | 2034.85 |

## Static scene

| Encoder | First UI frame B | Mean payload B/frame | Encode mean µs | Encode p95 µs | Trial mean range µs | Payload KiB/s at 30 FPS |
|---|---:|---:|---:|---:|---:|---:|
| imgui-quic | 64962 | 13.0 | 39.78 | 39.89 | 39.71–39.84 | 0.38 |
| imgui-quic-q16 | 45050 | 13.0 | 76.78 | 79.74 | 72.37–77.35 | 0.38 |
| imgui-quic+lz4 | 33183 | 13.0 | 39.82 | 39.97 | 39.72–42.36 | 0.38 |
| imgui-quic-q16+lz4 | 26801 | 13.0 | 75.60 | 79.67 | 72.41–81.17 | 0.38 |
| imgui-ws | 57408 | 12.0 | 20.75 | 20.68 | 20.66–20.78 | 0.35 |
| netImgui | 37616 | 160.0 | 3.67 | 3.68 | 3.66–3.70 | 4.69 |
| RemoteImGui | 22763 | 546.5 | 27.21 | 27.38 | 26.45–27.27 | 16.01 |
| raw | 64949 | 64949.0 | 0.97 | 1.01 | 0.95–0.99 | 1902.80 |

## Interpretation and limits

- `imgui-quic` is the exact-float codec; `q16` is the optional negotiated 1/16-unit relative-position/u16-UV codec.
- `+lz4` variants are encoder-only experiments with a hypothetical four-byte decoded-length header and uncompressed fallback. They are not supported by the live wire protocol. No end-to-end latency or packet traffic is claimed for these variants.
- imgui-ws uses its upstream default vertex-offset XOR/RLE encoder, adapted only for the Dear ImGui texture-ID accessor. Its diff size excludes incppect/WebSocket framing and uses 16-bit indices/32-bit texture IDs.
- netImgui uses the upstream conversion/compression functions and includes its command header. It quantizes positions to approximately 0.25-unit steps and UVs to u16, so it does not preserve the same geometry precision as the exact codec or q16.
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
python3 tools/benchmarks/comparison/run.py --frames 1500 --trials 7 --output build/comparison/all-modes.json
python3 tools/benchmarks/comparison/report.py build/comparison/all-modes.json docs/benchmarks/all-modes/README.md
```

- [Raw trials](results.json)
- [Build and adapter details](../../../tools/benchmarks/comparison/README.md)
- [ImGuiQuic exact/quantized decoder, memory and actual network measurements](../quantization/README.md)

Only ImGuiQuic has actual network and JavaScript decoder measurements in this experiment. Competing end-to-end traffic, CPU/RSS and input-to-display latency remain unmeasured; encoder results must not be presented as a full-product performance ranking.
