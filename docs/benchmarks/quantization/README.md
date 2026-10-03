# Quantization: cost, payload and network measurements

Quantization is implemented as an explicit negotiated option; exact geometry remains the default. LZ4 variants below are standalone encoder experiments and are not supported by the live client/server protocol.

## Encoding cost and payload

AMD Ryzen AI 9 HX 470, GCC Release, one logical CPU, the same pinned Dear ImGui and scenes as the [comparison report](../comparison/README.md). Five trials of 900 measured frames after 30 warmup frames. Rows show medians of trial statistics. Encoding includes serialization, packing, delta generation and any experimental compression; it excludes ImGui layout and transport.

| Scene | Mode | First frame B | Mean frame B | Encode mean µs | Encode p95 µs | Equivalent payload KiB/s at 30 FPS |
|---|---|---:|---:|---:|---:|---:|
| static | imgui-quic | 64962 | 13.0 | 39.04 | 38.99 | 0.38 |
| static | imgui-quic-q16 | 45050 | 13.0 | 74.11 | 77.47 | 0.38 |
| static | imgui-quic+lz4 | 33183 | 13.0 | 39.11 | 39.08 | 0.38 |
| static | imgui-quic-q16+lz4 | 26801 | 13.0 | 73.95 | 77.36 | 0.38 |
| moving | imgui-quic | 64962 | 44.0 | 43.59 | 44.62 | 1.29 |
| moving | imgui-quic-q16 | 45050 | 35.9 | 75.99 | 78.22 | 1.05 |
| moving | imgui-quic+lz4 | 33757 | 43.4 | 43.85 | 45.53 | 1.27 |
| moving | imgui-quic-q16+lz4 | 26802 | 35.9 | 75.14 | 78.31 | 1.05 |
| dynamic | imgui-quic | 64962 | 9003.4 | 44.66 | 46.64 | 263.77 |
| dynamic | imgui-quic-q16 | 45050 | 8540.6 | 87.08 | 91.35 | 250.21 |
| dynamic | imgui-quic+lz4 | 33183 | 4985.4 | 53.53 | 56.18 | 146.06 |
| dynamic | imgui-quic-q16+lz4 | 26801 | 4589.9 | 92.17 | 98.88 | 134.47 |

Equivalent payload rate is arithmetic, not captured network traffic. First-frame values exclude textures, authentication and transport handshakes. Static rows force an encode call on every frame; application-level unchanged-frame suppression is excluded. Encoder deltas assume the immediately previous frame is acknowledged.

Quantized vertices shrink from 20 to 12 bytes, but indices/commands stay unchanged. Therefore full-frame reduction is below 40%, and delta savings depend on content. Moving-window deltas shrink about 18%; dynamic-text deltas shrink only 5.1%, with approximately 95% higher encoding time. Exact + LZ4 reduces dynamic-text payload about 44.6% at approximately 20% higher encoding cost. Quantized + LZ4 reduces it about 49.0%, but approximately doubles encode cost and loses original float precision.

## JavaScript decoding and cache

Production JavaScript decoder executed under Node/V8 v26.10.0, CPU affinity 0; five trials of 300 measured frames after 30 warmup. Timing covers decode, validation and baseline-cache commit. Error checking is outside the timer. This measures JavaScript code, not browser WebGL/compositor time. Cache sizes count retained byte buffers only, excluding JS objects, fixtures and the separate expanded rendering buffer.

| Scene | Mode | Decode mean µs | Decode p95 µs | Baseline cache KiB | Expanded frame KiB | Max position error | Max UV error |
|---|---|---:|---:|---:|---:|---:|---:|
| moving | imgui-quic | 71.37 | 71.02 | 570.84 | 63.43 | 0 | 0 |
| moving | imgui-quic-q16 | 545.26 | 630.97 | 395.83 | 63.43 | 0.0250854 | 6.79493e-06 |
| dynamic | imgui-quic | 44.48 | 62.21 | 598.26 | 66.47 | 0 | 0 |
| dynamic | imgui-quic-q16 | 576.24 | 667.59 | 414.82 | 66.47 | 0.0250854 | 6.79493e-06 |

The separate adversarial cross-language fixture covers fractional positions, topology changes and exact/packed fallback transitions: 150 frames, 131 packed and 19 exact fallbacks. Observed maximum position error was 0.0200001 logical units and UV error 0.00000474602. The protocol position bound is 0.03125 logical units; physical pixel error scales with framebuffer scale. Texture IDs, colors, clipping and indices remain unchanged. Pixel-level screenshot comparisons were not measured.

## Live network: exact versus quantized

Two independent 8-second samples per mode/scene/profile, 30 target FPS, after warmup. Impaired netem profiles have a shared 2 Mbit/s budget. Exact baseline data comes from the preceding comparison run; quantized data was collected separately. These short runs have scheduling/congestion variance and are not paired randomized experiments. The Python receiver expands packed vertices, so latency includes its implementation cost and does not predict browser input-to-photon latency.

| Profile | Scene | Mode | FPS range | Wire KiB/s range | Server CPU % range | RSS MiB range | Input p95 ms range | Inputs observed/sent |
|---|---|---|---:|---:|---:|---:|---:|---|
| local | moving | exact | 30.00–30.00 | 11.23–11.33 | 1.75–2.12 | 11.85–11.88 | 9.79–14.84 | 70/70, 70/70 |
| local | moving | q16 | 29.99–30.00 | 10.67–10.76 | 2.00–2.50 | 11.44–11.84 | 15.61–26.24 | 70/70, 70/70 |
| local | dynamic | exact | 29.99–30.00 | 317.11–317.59 | 1.62–3.00 | 11.92–12.03 | 17.24–19.24 | 70/70, 70/70 |
| local | dynamic | q16 | 30.00–30.00 | 286.87–286.89 | 1.87–2.12 | 11.61–11.70 | 14.20–18.28 | 70/70, 70/70 |
| 100ms-1pct | moving | exact | 29.74–29.87 | 9.96–10.04 | 2.00–2.25 | 11.78–11.88 | 126.25–138.72 | 68/70, 68/70 |
| 100ms-1pct | moving | q16 | 29.74–29.87 | 9.32–10.14 | 2.25–2.37 | 11.83–11.94 | 138.58–139.67 | 70/70, 70/70 |
| 100ms-1pct | dynamic | exact | 10.75–10.87 | 122.30–122.51 | 1.87–2.50 | 11.27–11.98 | 249.77–297.48 | 52/70, 57/70 |
| 100ms-1pct | dynamic | q16 | 11.12–11.12 | 114.71–116.61 | 2.12–2.50 | 11.41–11.66 | 262.73–271.26 | 59/70, 56/70 |
| 300ms-3pct | moving | exact | 17.62–21.12 | 6.80–7.54 | 1.62–1.62 | 11.57–11.77 | 373.16–378.11 | 62/70, 49/70 |
| 300ms-3pct | moving | q16 | 16.62–19.87 | 5.52–6.66 | 1.62–1.75 | 11.70–11.84 | 369.91–387.78 | 56/70, 51/70 |
| 300ms-3pct | dynamic | exact | 2.87–3.87 | 34.41–44.05 | 1.25–2.00 | 11.57–12.04 | 587.91–988.30 | 24/70, 19/70 |
| 300ms-3pct | dynamic | q16 | 3.50–4.00 | 36.84–40.32 | 1.62–2.00 | 11.71–11.84 | 824.65–940.29 | 26/70, 18/70 |
| 100ms-5pct | moving | exact | 28.62–29.25 | 9.25–9.61 | 1.50–2.00 | 12.08–12.74 | 142.00–144.97 | 66/70, 64/70 |
| 100ms-5pct | moving | q16 | 27.37–28.87 | 9.24–9.34 | 1.62–2.37 | 11.82–12.04 | 142.23–143.06 | 66/70, 67/70 |
| 100ms-5pct | dynamic | exact | 6.87–8.00 | 76.13–89.06 | 2.00–2.75 | 11.55–11.72 | 378.30–493.44 | 50/70, 41/70 |
| 100ms-5pct | dynamic | q16 | 6.87–8.25 | 71.29–86.97 | 2.25–2.50 | 11.68–11.98 | 383.02–402.97 | 45/70, 35/70 |

Wire traffic is the isolated qdisc byte counter for the shaped QUIC flow, including both directions and protocol traffic. Input latency excludes unobserved/coalesced pointer updates; observation counts must be read alongside it. Raw results also include frame age p50/p95, sampled displayed-frame age p95, maximum frame gap, packet/drop counts, receiver handling time, datagrams and server context switches. Lower traffic at lower FPS is not automatically better compression.

Local dynamic-text wire traffic falls from about 317 to 287 KiB/s (about 9.6%) at 30 FPS. At 300 ms RTT and 3% loss, quantized dynamic text still delivers only 3.5–4.0 FPS, with 18–26 of 70 pointer updates observed. Quantization does not resolve bandwidth/ACK-window constraints by itself. There are no live LZ4 numbers because it is not a negotiated transport feature.

## Recommendation

Keep quantization opt-in. Prioritize a separately negotiated lossless compression experiment before enabling quantization by default. Its encoder-only savings are materially larger for this dynamic workload, while preserving float geometry. Visual acceptance and full browser performance still need evaluation at target DPI and texture sizes.

## Artifacts

- [Encoding trials](encode.json)
- [JavaScript decoder trials](decode.json)
- [Quantized network trials](network.json)
- [Exact network baseline](../comparison/network.json)
- [Reproduction commands and adapter boundaries](../../../tools/benchmarks/comparison/README.md)
