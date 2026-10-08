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
