# Integrated lossless draw encoding

The production encoder uses equal-block skipping and capability-negotiated
LZ4/XOR blocks. The browser negotiates lossless compression automatically;
geometry quantization remains optional. Measurements below include the full
codec selector, not a minimum assembled from independent experiments.

## Encoder

Five randomized trials, each with 30 warmup and 1500 measured frames, Release
GCC 16.2.1 on logical CPU 0 of an AMD Ryzen AI 9 HX 470. The 1280×720 scene has
one 1000×650 window and 30 text rows. Values are medians of trial statistics.
Serialization, allocation and selection are timed; layout and network I/O are
excluded. Both columns use the integrated equal-block scan; “legacy” disables
only the new compression capability.

| Scene | Mode | Bytes/frame | Encode mean µs | Encode p95 µs |
|---|---|---:|---:|---:|
| Static | Legacy exact | 13 | 3.09 | 3.10 |
| Static | Adaptive lossless | 13 | 3.08 | 3.08 |
| Moving window | Legacy exact | 44.1 | 7.48 | 8.00 |
| Moving window | Adaptive lossless | 44.1 | 7.55 | 8.05 |
| Changing text | Legacy exact | 8959.2 | 18.49 | 20.35 |
| Changing text | Adaptive lossless | 1144.0 | 15.15 | 22.95 |

Changing-text payload falls by 87.2%. Its median mean encoding time improves,
but p95 is slightly higher: selection/compression does not win on every frame.
The [pre-integration review](../encoding-review/README.md) measured approximately
45 µs for movement and 47 µs for changing text before block skipping; those are
historical runs, not simultaneous control measurements.

## Decoder

Five trials of 300 measured frames under Node/V8, with production decoding,
validation and cache commit. Correctness comparison is outside the timer.
These are Node VM timings, not browser render times.

| Scene | Mode | Decode mean µs | Decode p95 µs |
|---|---|---:|---:|
| Moving window | Legacy exact | 59.26 | 67.07 |
| Moving window | Adaptive lossless | 53.34 | 61.04 |
| Changing text | Legacy exact | 43.12 | 53.04 |
| Changing text | Adaptive lossless | 282.97 | 310.76 |

Moving packets use the same decoder path in both modes; their timing difference
is run variation. Dynamic compression trades additional client CPU for much
less traffic. At 30 FPS, 283 µs/frame is about 8.5 ms of CPU per second in this
Node test. No GPU/render timing is inferred from it.

## Actual QUIC traffic

One eight-second measured run per mode, scene and profile, plus bootstrap and
warmup. Native QUICHE host, independent Python/aioquic receiver, 30 FPS target,
10 Hz pointer input. Tests run sequentially in isolated user/network namespaces.
Impaired profiles apply the named RTT, jitter and random loss with a shared
2 Mbit/s queue; only the test's UDP port is shaped. The seed is 42. CPU frequency
and desktop background activity are uncontrolled. These short runs indicate
behavior and are not statistical confidence estimates.

Changing text:

| Network | Mode | Delivered FPS | Wire KiB/s | Frame age p95 ms | Observed input age p95 ms | Inputs observed/sent |
|---|---|---:|---:|---:|---:|---:|
| Local | Legacy | 30.00 | 316.47 | 2.19 | 13.99 | 70/70 |
| Local | Adaptive | 29.99 | 46.43 | 2.81 | 14.98 | 70/70 |
| 100 ms / 1% loss | Legacy | 11.12 | 126.72 | 160.44 | 257.07 | 55/70 |
| 100 ms / 1% loss | Adaptive | 29.37 | 41.10 | 60.73 | 137.67 | 70/70 |
| 300 ms / 3% loss | Legacy | 3.37 | 39.24 | 581.44 | 838.21 | 22/70 |
| 300 ms / 3% loss | Adaptive | 19.00 | 21.28 | 166.01 | 384.49 | 53/70 |
| 100 ms / 5% loss | Legacy | 7.75 | 85.86 | 286.27 | 372.46 | 47/70 |
| 100 ms / 5% loss | Adaptive | 28.50 | 39.02 | 65.58 | 143.03 | 67/70 |

Wire counters include both directions and retransmissions. Frame/input ages end
at the independent receiver's decoded-frame/ACK boundary, not browser scanout.
Only observed pointer sequences contribute to input-age percentiles; missing
updates must be considered alongside them. Measurements exclude connection and
texture bootstrap. Raw files also contain moving-window results, sampled display
age, native CPU/RSS, frame types and datagram counts.

The application credit remains eight frames / 24 KiB. The 300 ms case still
falls short of 30 FPS. Adaptive credit, independent cancellable frame streams,
shared multi-client encoding and direct packed GPU rendering are not part of
this implementation.

## Validation

- CTest: six suites, including 2000 randomized comparisons against the scalar
  patch encoder, incompressible fallback, motion preservation and mixed
  compression capabilities.
- Browser decoder: 240 cross-language byte-exact compression fixtures with aged
  baselines, changing lengths, packed/float transitions and recovery keyframes;
  malformed LZ4 bounds, cache immutability and presentation-before-ACK dispatch.
- Existing exact/quantized draw, WebTransport, development server, clipboard and
  media regressions passed.
- Real QUIC interoperability passed with compression enabled, disabled, and
  combined with quantization: authentication, loss/reordering, resource/epoch
  recovery, bounded bootstrap/windows, reconnect and large clipboard input.
- The local in-app browser displayed the demo through WebTransport; clicking
  incremented its counter, with no reported console errors. This is a visual
  smoke check, not a pixel-diff or browser performance benchmark.
- Installed CMake package linked and ran a separate C++ consumer. LZ4's license
  is included in the install tree. Local validation is x86-64; AArch64 runs are
  configured in CI but were not executed locally.

## Reproduce

Prepare the [comparison harness](../../../tools/benchmarks/comparison/README.md):

```bash
cmake -S tools/benchmarks/comparison -B build/comparison/build -DCMAKE_BUILD_TYPE=Release
cmake --build build/comparison/build -j 6
python3 tools/benchmarks/comparison/integrated.py
node tools/benchmarks/comparison/decode.mjs docs/benchmarks/integrated/decode.json
```

For network runs, first build `transport_benchmark_host` with
`-DIMGUI_QUIC_BUILD_BENCHMARKS=ON`, prepare development credentials and install
the independent receiver requirements as described in the [benchmark guide](../../../tools/benchmarks/README.md).
Then run each mode separately:

```bash
IMGW_BENCH_HOST_NETNS="$(readlink /proc/self/ns/net)" \
  unshare --user --map-root-user --net \
  build/webtransport_venv/bin/python tools/benchmarks/transport.py \
  --seconds 8 --trials 1 --output docs/benchmarks/integrated/network-lz4.json
IMGUI_QUIC_TEST_LZ4=0 IMGW_BENCH_HOST_NETNS="$(readlink /proc/self/ns/net)" \
  unshare --user --map-root-user --net \
  build/webtransport_venv/bin/python tools/benchmarks/transport.py \
  --seconds 8 --trials 1 --output docs/benchmarks/integrated/network-legacy.json
```

[Encoder trials](encode.json) · [Node decoder trials](decode.json) ·
[Adaptive network data](network-lz4.json) · [Legacy network data](network-legacy.json)
