# Encoding cost and traffic review

The highest-value next step is to accelerate exact range scanning, then add a negotiated lossless residual codec for changing geometry while retaining motion deltas for window movement. Quantization should remain optional until the browser can render its packed vertices directly. QUIC congestion control does not replace application-level frame scheduling.

This report records the encoder before lossless compression was integrated, including optional quantized geometry. See the [integrated measurements](../integrated/README.md) for the production selector. Experiments are isolated in the comparison harness; they do not enable a new live protocol. Source hashes and all trial results are in [results.json](results.json).

## Measurements

Five randomized trials per case, 1500 measured frames plus 30 warmup frames, Release GCC 16.2.1, logical CPU 0, AMD Ryzen AI 9 HX 470. The scene is a 1280×720 viewport, one 1000×650 window and 30 text rows. Values below are medians of trial means. Encoding includes canonical serialization, allocations and baseline ownership changes; it excludes ImGui layout, transport, decoding and rendering.

| Scene | Encoder | Payload B/frame | Encoding µs | Encoding p95 µs |
|---|---|---:|---:|---:|
| Static | Current exact | 13.0 | 40.40 | 40.70 |
| Static | Equal-block skip | 13.0 | 2.82 | 2.83 |
| Static | XOR + LZ4 | 282.0 | 10.34 | 14.95 |
| Window movement | Current exact | 44.1 | 45.13 | 47.36 |
| Window movement | Equal-block skip | 44.1 | 7.52 | 8.11 |
| Window movement | Equal-block skip + LZ4 | 43.5 | 7.68 | 8.27 |
| Window movement | XOR + LZ4 | 3459.5 | 15.92 | 25.92 |
| Changing text | Current exact | 8959.2 | 47.28 | 48.96 |
| Changing text | Equal-block skip | 8959.2 | 18.75 | 21.08 |
| Changing text | Equal-block skip + LZ4 | 4961.9 | 27.70 | 30.81 |
| Changing text | XOR + LZ4 | 1164.4 | 14.40 | 17.11 |

Equal-block skipping reduces moving-scene encoding time by 83.3% and changing-text encoding time by 60.3%, with identical output. XOR + LZ4 reduces changing-text payload by 87.0% and encoding time by 69.5% relative to the current exact encoder. At 30 FPS, that changing-text payload is approximately 34.1 KiB/s instead of 262.5 KiB/s, before transport overhead. These are separate candidate results; an adaptive encoder that evaluates multiple candidates will have additional selection cost.

The XOR candidate copies the current canonical frame, XORs it with the available baseline, and compresses the result with RemoteImGui's bundled LZ4. New bytes use a zero baseline; truncation is represented by decoded length. Reported sizes include an estimated 13-byte frame envelope and a four-byte length envelope when compression wins. These are estimated application payload sizes, not a negotiated wire format or measured network traffic.

A separate `perf record -e cpu-clock:u` run of 15000 changing-text frames attributes **81.22% of sampled user CPU to `encode_ranges`**. This profile includes scene generation, not just the timed encoder section. See [perf-current.txt](perf-current.txt). No kernel/network CPU attribution is implied.

## Findings and implementation order

### 1. Remove the scalar equal-byte scan

[`encode_ranges`](../../../src/draw_protocol.cpp) scans unchanged bytes individually. Its patch-building loop must inspect changes carefully, but long equal stretches do not require scalar traversal. The experiment compares 32-byte blocks with `memcmp` and skips equal blocks before entering the existing scalar logic. It retains exact patch boundaries and tie rules.

This is the first production candidate: large measured CPU savings, unchanged protocol and no browser work. Generated fixture output from the current and experimental binaries is byte-identical for 300 measured frames in each scene, including the canonical originals. The experiment does not constitute exhaustive proof. Before integrating, add randomized baseline/length/change-pattern equivalence checks, run the existing I/P and transport regressions, and measure AArch64. Do not assume an x86 compiler's code generation transfers unchanged to ARM.

### 2. Add lossless compression before spending more precision

Range patches pay eight bytes per changed span and transmit changed bytes verbatim. Repetitive numeric text produces many such spans. Compressing an XOR residual avoids that patch metadata and turns unchanged data into zero runs. It performs substantially better on this workload than compressing the already-built patch packet.

Retain the current motion predictor: replacing it with whole-frame XOR increases moving-window payload from 44 to 3460 bytes. A candidate policy is:

1. Preserve unchanged-frame suppression and recovery I frames.
2. Detect and encode motion; finish immediately when the exact residual is small.
3. For larger residuals or changing content, try XOR + LZ4 with uncompressed fallback.
4. Bound candidate work using measured size thresholds or recent codec results; benchmark the complete selector, not the minimum of independently measured candidates.

The new codec needs capability negotiation, explicit decoded-size limits and baseline identity. Its output must reconstruct canonical bytes exactly, including NaNs or arbitrary bit patterns wherever the current protocol allows them. Bootstrap compression, decode cost, malformed input and recovery need independent coverage. Browser decoding could use a small WASM codec, but that adds deployment and initialization costs that have not been measured here.

The harness verifies LZ4 round trips and independently reconstructs XOR output against the original frame for all three scenes. It assumes the immediately preceding baseline is available. Real ACK-delayed baselines can be much older and must be benchmarked before choosing thresholds. No browser decoder for this candidate exists yet.

### 3. Tune frame credit for RTT after reducing payload

[`State::publish_draw_frame`](../../../src/state.cpp) limits pending frames to eight and pending encoded bytes to 24 KiB. [`send_native`](../../../transport/quiche/server.cc) separately enforces eight outstanding frames. These bounds protect freshness and memory, but constrain high-RTT delivery independently of QUIC's congestion window.

At 300 ms RTT, eight frames imply an idealized 26.7 FPS ceiling; 24 KiB implies about 80 KiB/s of outstanding application-payload turnover. With roughly 9 KiB frames, only two fit at once. These are simple credit/RTT estimates, not throughput predictions: actual baselines, loss and ACK processing change the outcome. Existing [network measurements](../quantization/README.md) show substantial dynamic-scene FPS reduction under impairment.

Compress first. Then evaluate bounded credit derived from RTT, recent frame size and a freshness target. Change both application and bridge limits together. Keep latest-unsent replacement and acknowledge presentation, never receipt. Increasing queue capacity alone can increase stale-frame latency.

Frames larger than the negotiated datagram payload use the same reliable ordered output stream as resource/control records. Loss of earlier stream bytes can delay subsequent data on that stream. Independent cancellable frame streams are a second-stage experiment; they must preserve texture/control barriers and bounded memory. A mean compressed frame size near a datagram limit does not guarantee that every frame fits. Measure size percentiles and actual stream/datagram ratios before drawing transport conclusions.

### 4. Avoid expanding quantized vertices on the browser CPU

[`expandQuantized`](../../../frontend/imgui_quic_draw.js) reconstructs 20-byte float vertices from 12-byte packed vertices before [`frontend/imgui_quic.js`](../../../frontend/imgui_quic.js) uploads them. This spends CPU and forfeits the GPU-upload size benefit. Prior Node/V8 measurements show roughly 0.55–0.58 ms per quantized frame versus 0.04–0.07 ms for exact decoding in these scenes. Node VM timings are diagnostic evidence, not browser rendering measurements.

A direct packed-vertex path can use signed-short positions with origin and 1/16 scale in the vertex shader, normalized unsigned-short UVs, and normalized byte colors. That would reduce vertex-buffer bytes by 40% and remove CPU float materialization. Keep structural validation, exact-format fallback and draw-offset correctness. Verify actual browser timing and rendered output before changing the default.

Server quantization also needs attention: repeated scalar rounding, vector growth, and work performed before checking whether the client can receive a frame. Reserving capacity and moving eligibility checks earlier are lower-risk candidates; their gains remain unmeasured.

### 5. Share encoding across compatible clients and shorten lock hold time

`State::publish_draw_frame` holds `clients_mtx_` across quantization, geometry comparison, encoding and allocation for every client. Clients with the same acknowledged baseline and negotiated format repeat the same work. It can also finish encoding a packet that is then rejected by the remaining byte budget.

Cache encoded results by current snapshot, acknowledged baseline and codec capabilities; share immutable output where those match. Snapshot eligible client state under the lock, encode outside it, then revalidate client lifetime/generation, baseline and credit before publication. Preserve frame order, mailbox notifications and resource barriers. Do not reuse a delta for a client with another baseline.

Measure one, four and eight clients with both synchronized and staggered ACKs. This finding is from source inspection; the current experiment does not quantify contention or multi-client CPU savings.

## Comparison scope and remaining evidence

The [upstream encoder comparison](../all-modes/README.md) has netImgui at about 4.45 µs / 10.7 KB for changing text and RemoteImGui at about 31.25 µs / 1.36 KB. Those codecs use different geometry precision/layouts, and those numbers are from a separate run. The exact XOR experiment is promising on this corpus, but it does not establish a general product ranking or the lowest possible CPU cost.

Before selecting a default, extend the corpus to plots, tables, large meshes, topology churn, texture updates and multiple windows. Measure ACK baseline ages, 30/60 FPS, actual browser decode/upload/render CPU, presentation age, observed input updates, wire bytes including retransmissions, native CPU and peak memory. Repeat on AArch64. Static encoder tests force repeated calls; the live application already suppresses unchanged geometry.

Only ImGuiQuic currently has impaired-network measurements. Competing viewers and complete networking stacks were not exercised. Font textures and connection bootstrap are excluded from codec sizes, and CPU frequency/background desktop activity are uncontrolled. No production codec or transport policy was changed by these experiments.

## Reproduce the comparison

The current harness uses LZ4 1.10.0 and a generated scalar reference. Fresh
results go to `build/comparison/review.json`; they do not overwrite this
historical report, which used RemoteImGui’s bundled LZ4.


Prepare the pinned upstream checkouts using the [comparison instructions](../../../tools/benchmarks/comparison/README.md), then:

```bash
cmake -S tools/benchmarks/comparison -B build/comparison/build -DCMAKE_BUILD_TYPE=Release
cmake --build build/comparison/build -j 6
cmake -S tools/benchmarks/comparison -B build/comparison/scalar -DCMAKE_BUILD_TYPE=Release -DEXPERIMENT_SCALAR_SCAN=ON
cmake --build build/comparison/scalar -j 6
python3 tools/benchmarks/comparison/review.py
perf record -q -e cpu-clock:u -o build/comparison/review.perf -- taskset -c 0 build/comparison/build/codec imgui-quic dynamic 15000
perf report --stdio -i build/comparison/review.perf --percent-limit 2 --sort symbol
```

The scalar-reference build generates a modified source copy only inside its build directory. Verification runs are separate from timed runs. Raw results retain medians' constituent trials and fixture hashes.
