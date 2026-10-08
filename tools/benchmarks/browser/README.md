# Chromium end-to-end benchmark

This opt-in benchmark launches the **production frontend, decoder and WebGL
renderer** in Chromium against `transport_benchmark_host`. WebTransport, draw
codec, texture barriers and submit-before-ACK semantics stay unchanged. A
benchmark-only stdin/stdout clock probe in the native host is the only added
native behavior. No transport messages or public APIs are added.

## Run on RK3588 + Chromium (native Linux AArch64)

Prerequisites: Node 22+, Python 3 (tests only), OpenSSL CLI, CMake 3.21+, GCC,
Clang/C++20, ICU and OpenSSL development files, and Chromium with WebTransport
and WebGL. Use the board's existing graphics drivers and a visible desktop
session for GPU/display-opportunity measurements. Record governor, temperatures,
display refresh rate and driver version with the results; keep them fixed across
comparisons. First QUICHE build requires network access.

From the repository root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DIMGUI_QUIC_NATIVE_QUIC=ON -DIMGUI_QUIC_BUILD_BENCHMARKS=ON \
  -DIMGUI_QUIC_BUILD_TESTS=ON -DIMGUI_QUIC_BUILD_EXAMPLES=ON
cmake --build build -j4
npm install --prefix build/browser --save-exact playwright@1.62.1

# System Chromium on RK3588: no downloaded x86 browser, no SwiftShader flags.
node tools/benchmarks/browser/run.mjs --chromium /usr/bin/chromium --headed \
  --scene moving --seconds 30 --trials 3 \
  --output build/benchmarks/browser/rk3588-moving.json
node tools/benchmarks/browser/run.mjs --chromium /usr/bin/chromium --headed \
  --scene dynamic --seconds 30 --trials 3 \
  --output build/benchmarks/browser/rk3588-dynamic.json
```

If the distribution calls the binary `chromium-browser`, substitute its path.
For desktop/headless functional runs, install a Playwright browser with
`node build/browser/node_modules/playwright/cli.js install chromium` and omit
`--chromium --headed`. Headless/software rendering does **not** establish board
GPU or physical display performance. JSON records browser version, GL renderer,
context attributes, architecture, kernel, commit/dirty state and host SHA-256.
Use `--no-sandbox` only in a deliberately isolated root container that cannot run
the Chromium sandbox. Default launches enable the sandbox. An existing Playwright
installation can be selected using `IMGUI_BENCH_PLAYWRIGHT=/absolute/module/path`.

The harness owns UDP 19443, an ephemeral loopback HTTP port, fresh 13-day ECDSA
credentials, a random token, the native process and the browser. Run sequentially;
a busy UDP port is a failed run. Exact page Origin and certificate pin are used;
no global certificate verification bypass. Credentials are removed on completion
and excluded from JSON. Trials use fresh processes, 1280x720 CSS viewport, DPR 1,
exact draw precision, 30 native FPS, one-second warmup, and three trials by default.
Use `--host` for another build directory. No network shaping is applied.

## Clock contract

Native colors retain the original **64-bit steady_clock nanoseconds**, exported
as decimal strings to avoid JSON integer truncation. Browser stages use
`performance.now()` milliseconds. `performance.timeOrigin` is metadata only;
wall time is never used to align clocks.

For each probe, the browser records `b0` before `/clock` and `b1` after consuming
its response. The local HTTP harness writes `c` to the host's stdin; the host
samples its actual `steady_clock` as `n` and flushes it to stdout. Hence the native
sample occurred inside the browser interval. With `offset = browser - native`:

```
b0 - n <= offset <= b1 - n
```

Units are converted to ms. This bound needs **no symmetric RTT assumption** and
includes HTTP, process scheduling and IPC overhead. Twelve probes before and
after measurement plus one per second are preserved in JSON. For each measured
endpoint, intervals are expanded by `--resolution-ms` (default 1 ms per bound)
and `--drift-ppm` (default 100 ppm times distance from the probe), then intersected.
Empty intersections fail the trial. These allowances are explicit assumptions,
not a measured guarantee about browser timer precision, clock drift or suspend.
Do not suspend the machine during a run. Increase the allowances if the platform's
clock contract requires it; do not silently adjust them to pass a failing run.

Each cross-clock latency contains midpoint, lower and upper values. The maximum
half-width is `clock_uncertainty_ms`. Negative midpoints inside the interval are
retained, not clamped; generation after receipt even at the upper bound fails.
The host and Chromium must run on the **same machine** in this minimal harness.
Remote-board clients require a separately secured clock endpoint/runner extension.
No assumption that Python/Node and C++ monotonic clocks share an epoch is made.

## JSON fields and measurement boundaries

`trials[].samples.rows` holds per-frame raw samples; `metrics` holds n/p50/p95/max
in ms, using sorted nearest-rank-style index `ceil((n-1)*p)`. Missing values remain
`null`, never zero. Trial failures remain in the output and cause exit code 1.

| Field | Meaning |
| --- | --- |
| `native_ns` | Host marker before drawing the workload; includes native generation/encode/queue costs afterward |
| `receive_ms`, `path` | Browser complete application record/datagram at MixedReceiver entry, before envelope checks; **not NIC/kernel packet arrival**; reliable-stream reassembly is already complete |
| `decode_start_ms`, `decode_end_ms` | Production I/P decompression, delta reconstruction, expansion and validation |
| `submit_start_ms`, `submit_end_ms` | CPU wall time around production `renderFromParsed`, including WebGL buffer uploads and calls; return does not imply GPU completion |
| `parse_and_prepare_ms` | Decode end to submit start: geometry parsing, texture checks **and observer overhead** |
| `gpu_elapsed_ms`, `gpu_status` | Optional asynchronous WebGL1 `EXT_disjoint_timer_query` interval around rendering; GPU command execution interval, not CPU duration, absolute GPU completion time or presentation |
| `raf_callback_ms` | `performance.now()` at the first RAF callback scheduled after submit |
| `raf_timestamp_ms` | RAF-supplied timestamp, kept separately; not substituted for actual callback entry |
| `next_raf_proxy_ms` | Entry into the following RAF: display-opportunity proxy only, **not compositor present/scanout** |
| `superseded_before_raf`, `superseded_before_proxy` | Whether a newer submitted frame has replaced this frame by each callback |
| `compositor_present_ms` | Always null: no per-frame compositor presentation API used |
| `generation_to_{receive,submit,raf_proxy}_{ms,lower_ms,upper_ms}` | Cross-clock latency estimate and interval bounds |

RAF schedules observations only; it does not move rendering into RAF or delay the
ACK. Several updates can overwrite the same canvas between refreshes. All raw
rows are retained, but proxy percentiles exclude superseded rows. Consequently
proxy sample count can be much smaller than submitted frame count, and excludes
no guarantee of presentation. GPU queries are polled after returning to the event
loop; no busy wait, `gl.finish`, `readPixels` or synchronous GPU completion is used.
Unsupported, pending, disjoint or exhausted-query-budget values remain null with
status. Any observed disjoint invalidates GPU durations for the whole trial.
Queries and observations add overhead: treat these as instrumented results.
The production `preserveDrawingBuffer: true` context remains unchanged.

CPU submit, GPU elapsed, RAF opportunity and physical presentation must never be
added or relabeled as interchangeable stages. For actual photon/scanout latency,
use a separately synchronized external measurement or a validated platform-specific
presentation trace with frame attribution; neither is claimed here.

Specification references:
[WebGL timer query](https://registry.khronos.org/webgl/extensions/EXT_disjoint_timer_query/),
[HTML animation frames](https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#animation-frames).

## Pass/fail

Default PASS requires every trial to satisfy:

- No page exceptions, decode/recovery errors, unexpected transport close, WebGL
  errors/context loss, or hidden page during measurement.
- Strictly increasing frame IDs, complete ordered receive/decode/submit/RAF samples.
- At least max(10, floor(seconds × requested FPS × 0.8)) samples and measured
  throughput at least 80% of requested FPS (24 FPS for the default workload).
- Consistent calibration; max clock half-width ≤ 5 ms (`--max-uncertainty-ms`).
- CPU submit p95 ≤ 16.7 ms (`--max-submit-ms`). This is an explicit initial budget,
  **not an established RK3588 performance result**.

An optional `--max-proxy-ms 100` gates p95 of the **upper** latency bounds for
non-superseded RAF proxy samples; zero eligible samples fail that gate. Without
this option proxy latency is descriptive, not gated. Set a product-specific
budget before comparing runs. GPU unsupported/disjoint is separately reported
and does not fail CPU/transport validity; GPU claims require valid GPU samples
and a verified hardware renderer. Headless PASS is a functional/instrumented
result only. No result here can PASS a physical-display-latency requirement.

## Regression commands

```bash
ctest --test-dir build --output-on-failure --no-tests=error
node tests/browser/benchmark.mjs
python3 tests/browser/benchmark_clock.py build/transport_benchmark_host
node tests/browser/webtransport.mjs
node tests/browser/draw_ip.mjs
node tests/browser/compressed_draw.mjs
node tests/browser/planar_draw.mjs
node tests/browser/quantized_draw.mjs
```

`benchmark.mjs` uses deterministic clock/RAF/GPU doubles to check offset bounds,
asymmetry, drift budgets, inconsistent probes, missing samples, frame ordering,
marker extraction, supersession and disjoint invalidation. Those are correctness
tests, not browser latency measurements. `benchmark_clock.py` runs the real host
without TLS/clients to verify replies and shutdown with stdin still open; it also
works with `IMGUI_QUIC_NATIVE_QUIC=OFF` for core-only testing.
