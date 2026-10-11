# Validation recorded for this change

Base: `74066ea8add04ea97ee9e5d9acab768df44bdd8a` (main).
Environment: Linux x86-64, GCC 13.3.0, Node 24.19.0. Core-only Release build;
no RK3588, Chromium executable, or Clang available in this workspace.

## PASS — executed

- `cmake --preset core-tests -DIMGUI_QUIC_BUILD_BENCHMARKS=ON` and
  `cmake --build --preset core-tests`: compiled core, examples, tests and the
  modified timestamped benchmark host. CMake was invoked from an existing local
  installation because it was absent from PATH.
- `ctest --preset core-tests --no-tests=error`: **7/7** passed (protocol, C API,
  draw protocol, QUIC protocol logic, launcher credential preparation, server
  configuration, quantization).
- `python3 tests/browser/benchmark_clock.py build/core-tests/transport_benchmark_host`:
  real native clock replies, ignored unknown input, monotonic values, termination
  while the stdin pipe remains open.
- **9 Node regression scripts**: benchmark, clipboard_shortcuts, compressed_draw,
  draw_ip, large_clipboard_batch, media_viewer, planar_draw, quantized_draw,
  webtransport. New cases check clock bounds and drift, missing/invalid samples,
  marker extraction, superseded RAF observations, GPU disjoint invalidation,
  complete-record metadata, and unchanged submit-before-ACK ordering. Existing
  codec fixtures cover 302 I/P, 240 compressed, 240 planar and 450 quantized frames.
- JavaScript syntax checks and `git diff --check`.
- Harness negative path: invoking the core-only host with TLS credentials writes
  machine-readable FAIL JSON and exits 1; diagnostic is `Native QUIC disabled in
  this build`. No synthetic latency samples were substituted.

## BLOCKED / NOT RUN

- Native QUICHE build was attempted with `IMGUI_QUIC_NATIVE_QUIC=ON`. It stopped at
  `cmake/BuildQuiche.cmake:8`: `Could not find CLANG using the following names: clang`.
- Playwright 1.62.1 is installed, but launching Chromium failed because the expected
  `chromium_headless_shell-1234/.../chrome-headless-shell` executable does not exist.
- `tests/browser/dev_server.mjs` cannot complete with the core-only build: its
  supervised native example exits when QUIC is unavailable, and the HTTP request
  sees `ECONNREFUSED`. The credential-only launcher CTest did pass. This is a
  build-configuration/environment limitation, not reported as a frontend PASS.
- Real Chromium → native QUICHE session, driver GPU timer results, RK3588 throughput
  and latency, and physical display measurements: **NOT RUN**. The harness must
  still be run on a machine meeting README prerequisites before accepting an
  end-to-end performance claim.

The deterministic browser/GL doubles validate measurement logic only. No board
or browser performance numbers are claimed in this change.

# Follow-up acceptance — 2026-10-12 (Asia/Shanghai)

Tested PR head: `9d2a9ea2a769ce475d1f5d517b4171fff3124544`.
GitHub tested merge: `0fb54f9c36329f13a9651750913f5155d1035f65`.
[Executed CI run](https://github.com/n3in2019/imgui-quic/actions/runs/38160075454).
[Raw artifact](https://github.com/n3in2019/imgui-quic/actions/runs/38160075454/artifacts/11710457365).

## PASS — real Chromium, not a test double

Ubuntu 24.04.5 x86-64 / kernel 6.17.0-1022-azure; Clang 18.1.3,
Node 22.23.3, Playwright 1.62.1, Chromium 151.0.7922.34. Real native
QUICHE host, production WebTransport adapter, decoder and WebGL submission.
Chromium sandbox remained enabled; a root-owned AppArmor profile allowed user
namespaces only for the exact installed Chromium executable paths. No global
certificate bypass, global user-namespace policy disable, or no-sandbox fallback.

Both scenes ran for five seconds with one fresh-process trial each:

| Scene | Frames | FPS | CPU submit p95 ms | Max clock half-width ms | RAF proxy upper-bound p95 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| moving | 150 | 29.9210 | 0.2000 | 2.0439 | 20.3011 |
| dynamic | 150 | 29.9216 | 0.2000 | 2.0025 | 21.4838 |

Each trial preserved 29 clock probes, all 150 complete ordered stage rows, and
all metric sample counts. No reported page/transport/decode/WebGL errors,
no inconsistent interval intersections and no superseded proxy samples.
Independent recomputation checked all 900 cross-clock endpoint intervals against
the raw 58 probes and verified stage ordering and null compositor-present values.
Bounds remain conditional on the explicit 1 ms resolution and 100 ppm drift
allowances; these short runs do not establish oscillator/suspend guarantees.
The generation-to-receive midpoint is smaller than the roughly 2 ms calibration
half-width: it must not be advertised as a precise sub-millisecond latency.

Renderer: ANGLE / Vulkan / **SwiftShader**. All 300 GPU statuses were
`unsupported`; GPU elapsed metrics had n=0 and null percentiles. All 300 actual
compositor-present values were null. The second-RAF values above are opportunity
proxies only. This is headless software-rendering functional/instrumented
acceptance, not hardware GPU or physical presentation acceptance. CI used its
existing CMake configuration without an explicit Release build type; QUICHE used
Bazel `-c opt`. Use the documented Release configuration for board baselines.

Both x86-64 and ARM Linux native CI jobs passed CTest, frontend regressions and
native WebTransport integration. ARM CI is not an RK3588 GPU test. At this
record's completion, the separate vcpkg packaging job was still running; no
package/release acceptance is inferred from the browser result.

## Executed failures and fixes

First follow-up run:
[CI](https://github.com/n3in2019/imgui-quic/actions/runs/38158284018),
[failed JSON artifact](https://github.com/n3in2019/imgui-quic/actions/runs/38158284018/artifacts/11709439876).
Both JSON reports correctly contained FAIL at `chromium_launch` (Ubuntu AppArmor
user-namespace restriction / no usable sandbox), but the CI step incorrectly
returned success because the implicit `bash -e` shell did not enable pipefail
and `tee` masked the runner's exit 1. This run is **not** browser acceptance.

Fixed in the existing PR:
- Explicit Bash pipefail and JSON PASS/frame-count verification. The exact
  workflow script with a core-only host now retains both FAIL reports and exits 1.
- Scoped AppArmor setup for the downloaded Chromium executables, following
  [Chromium's official guidance](https://chromium.googlesource.com/chromium/src/+/main/docs/security/apparmor-userns-restrictions.md).
- Invalid/unknown CLI options now write FAIL JSON and exit 1. New CLI regression
  also checks a missing host independently of Playwright availability.
- Missing stage timestamps no longer become zero-duration samples. Missing
  measurement boundaries leave duration and FPS null, with FAIL.
- Nonfinite clock probes/endpoints are rejected. Failed trials preserve their
  phase, completed clock probes, browser console errors and native exit state
  observed before runner-initiated cleanup.

The old code was exercised: invalid arguments exited 1 without creating JSON;
null submit timestamps produced n=1/p95=0. The fixed code writes the error report
and produces n=0/null percentiles. These are observed defects, not hypothetical
performance changes. Valid sample timing and wire/ACK behavior remain unchanged.

## Local Linux dependency and native validation

Ubuntu 24.04.3 x86-64, GCC 13.3.0, extracted Clang 18.1.3/ICU development files,
CMake 4.4.4, Node 24.19.0. System package installation was unavailable; packages
were downloaded and extracted into the ignored build directory. Chromium's CDN
route returned HTML; the official Chrome-for-Testing storage download succeeded
and its ZIP was verified.

The Bazel daemon could not start in this container. A batch build succeeded with
the system Java CA trust store, an ICU-only builtin include path, and a temporary
sandbox directory with reuse disabled. The pinned QUICHE/Bazel revisions did not
change. Full native build, 7/7 CTest, native clock probe, 10 Node regression scripts
and a real native QUIC integration run passed (44 frames / 55 datagrams plus
origin/auth rejection, recovery and bounded ACK checks).

The complete native benchmark host listened on its UDP port and answered clock
readiness. Chromium then failed on Unix socket creation with `Operation not
permitted`, even with the explicit isolated-container no-sandbox option. The
runner retained FAIL JSON at `chromium_launch` and exited 1. No local browser
performance data was substituted. Exact commands, environment, build logs,
original/fixed regression records and both CI artifacts are retained in the
acceptance evidence package.

## Draft and remaining evidence

The real-browser prerequisite for ending Draft is now met. This PR can proceed
to review as measurement tooling. It is not yet a trusted RK3588 performance
baseline: both scenes still need the documented 3 x 30 s board runs, fixed
power/thermal/display conditions and a verified driver. Hardware GPU elapsed
claims additionally need supported, nondisjoint GPU timer samples. Real
scanout/photon latency still needs external measurement or a validated
frame-attributed presentation trace. The software CI smoke cannot supply those
results.
