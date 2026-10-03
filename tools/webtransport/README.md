# Native WebTransport

HTTP/3, QUIC and TLS run inside the native application. CMake links the Google QUICHE C++
endpoint (`libimgui_quic_quiche.so`) with BoringSSL into the application.
The native process owns only the QUIC listener. Frontend files are hosted separately.

## Local development

Requires Linux x86-64 or little-endian AArch64, CMake, a C++17 core compiler,
Clang with C++20 support, ICU development files (`libicu-dev` on Ubuntu), and OpenSSL development libraries (`libssl-dev` on Ubuntu). Pinned Bazel 8.2.1 is downloaded with SHA-256 verification. The C++ development launcher generates local certificates and tokens automatically.

```bash
cmake -B build -DIMGUI_QUIC_BUILD_EXAMPLES=ON -DIMGUI_QUIC_BUILD_TESTS=ON
cmake --build build -j4
./build/examples/imgui_quic_dev
```

Open the URL in `build/webtransport_dev/url.txt`. The status indicator says
**connected (WebTransport)**. The launcher prepares credentials, serves the frontend
on TCP 127.0.0.1:4433 and supervises the native QUIC example on UDP 127.0.0.1:4433.
Ctrl-C stops both. Use `--address` and `--port` to select the listening interface
and shared TCP/UDP port. `--frontend` selects the asset directory; `--page-url`
uses an external frontend host instead of starting the local HTTP server.
`--build-directory` and `--credentials` select build and credential directories.
A QUIC startup failure stops the launcher and its HTTP listener.

The launcher creates an ECDSA certificate valid for 13 days, its SHA-256 pin,
and a random token with private file permissions. Existing credentials are
reused. Remove only those development files and rerun to rotate expired
credentials. No browser flags or system trust changes are needed when the
browser supports `serverCertificateHashes`.

URL fragment parameters are `wt-url`, `wt-token`, and optional `wt-cert` (64 hex
SHA-256 digits). The fragment is not sent with the HTTP page request but remains
a credential in browser history. Do not share or log the launch URL. The token
travels in the first encrypted application record.

With existing certificates, the example runs directly:

```bash
IMGUI_QUIC_CERT=build/webtransport_dev/cert.pem \
IMGUI_QUIC_KEY=build/webtransport_dev/key.pem \
IMGUI_QUIC_TOKEN_FILE=build/webtransport_dev/token \
IMGUI_QUIC_ORIGINS=http://127.0.0.1:8888 \
IMGUI_QUIC_PORT=4433 \
./build/examples/example_core_cpp_draw
```

## Application integration

After initializing the core, before accepting clients, call
`imgui_quic_start()` from `include/imgui_quic_transport.h`. Provide certificate,
private-key and token file paths plus exact allowed page origins. The config is
copied. `imgui_quic_shutdown()` closes QUIC and joins its workers before freeing
shared state.

The CMake option `IMGUI_QUIC_NATIVE_QUIC` defaults ON. OFF builds omit QUICHE
and reject `imgui_quic_start`. Installed CMake packages include the private
shared library and dependency license notices. Only its two C ABI entry points
are exported, isolating QUICHE/BoringSSL/Protobuf symbols from the application.
Ship the shared library beside your installed libraries; `find_package` links it.
QUICHE is pinned in `transport/quiche/dependencies.json`; its upstream Bazel
module lock file fixes transitive dependencies. The initial build needs network
access and takes longer than incremental builds. `IMGUI_QUIC_QUICHE_JOBS` limits
compiler parallelism (default 6).

### ARM

Use the same CMake commands on a **native Linux AArch64** machine. The bootstrap
selects Bazel's arm64 binary; QUICHE/BoringSSL select native ARM code without x86
compiler flags. CI runs the core, frontend and real QUIC interoperability tests
on `ubuntu-24.04-arm` as well as x86-64. ARM32, big-endian ARM, and cross compilation
are not configured; CMake rejects unsupported targets instead of building a host
library accidentally. AArch64 CI coverage is configured; local verification was
performed on x86-64 and does not establish an ARM test result.

Native mailboxes signal a shared Linux eventfd when a frame or control message
is published. QUICHE waits for that signal, socket events and its own alarms;
a 100 ms watchdog checks application deadlines. Budget-limited buffered work is
resumed immediately, so large records do not wait for another network packet.
The notification descriptor remains alive while any producer retains a mailbox,
and shutdown explicitly wakes and joins the I/O thread.

Reliable records are assembled directly from the immutable native packet, with
one adapter payload copy. On little-endian x86-64/AArch64, canonical ImGui vertex
bytes are copied in bulk; custom layouts retain the scalar serializer. Motion
encoding evaluates its residual first and stops building the ordinary candidate
once it is provably larger. Wire bytes and the strict smaller-candidate rule are
preserved; no lossy geometry quantization or floating-point fast-math is used.

## Delivery and limits

- Small P/motion-P frames and pointer moves use datagrams, capped at the client
  limit, the QUIC path limit, and 1100 bytes. No application fragmentation.
- I frames, larger P frames, textures and critical inputs use a reliable bidi
  stream. Independent cancelable frame/resource streams remain future work.
- Native `State` is accessed directly through a small internal C ABI. One resource
  is outstanding until its application ACK. Control versions and recovery epochs
  prevent stale geometry from presenting with the wrong resources.
- Presentation/ACK IDs only advance. An ACK requires a frame actually sent in the
  current epoch. Eight outstanding frames and nine client snapshots bound baseline
  dependencies; the native encoded-byte window is 24 KiB, with one oversized frame
  allowed alone. Bootstrap/recovery I frames are single-flight.
- Datagram-only stalls recover after two seconds; reliable frames use QUIC
  retransmission. Resource/bootstrap ACKs allow 60 seconds. Established native
  ACK stalls disconnect after ten seconds. Recovery is rate limited to once/second.
- Reliable input carries pointer position and sequence fences. Button releases,
  text, wheel and resize remain reliable. Pointer moves are supersedable.
- The endpoint defaults to eight admitted connections and two per IP, including
  handshakes. TLS handshakes, opening a WebTransport session, and its
  combined stream/token authentication phase have five-second deadlines; capability negotiation has a 15-second deadline. Input
  record bytes are bounded by 64 MiB + 1024 per session, with bounded output queues and per-tick input budgets.
- The browser retries only WebTransport with 1–15 second backoff. Unsupported
  browsers, invalid configuration and blocked UDP produce a connection error.

## Deployment

Use a trusted certificate and an HTTPS page remotely; omit the certificate pin
when using normal browser trust. Allow UDP to the native QUIC port. Origins are
an exact comma-separated allowlist. Shared tokens do not provide per-user identities.

## Tests and benchmarks

The independent interoperability client is an optional Python development tool.

```bash
python3 -m venv build/webtransport_venv
build/webtransport_venv/bin/pip install -r tests/webtransport/requirements.txt
ctest --test-dir build --output-on-failure
node tests/browser/webtransport.mjs
build/webtransport_venv/bin/python tests/webtransport/test_native.py
```

The independent aioquic client talks directly to the native binary. It checks
Origin/token rejection, native popup input, datagrams, reordered/dropped frames,
bounded queues, reconnect and recovery are tested against the native endpoint.
See [benchmarks](../benchmarks/README.md) for reproducible netem measurements.

### Starter application

Run `./build/examples/imgui_quic_dev --example minimal`
for the small counter application in `examples/minimal.cpp`. `--example demo`
(the default) starts the full widget/media regression example. Both share
`examples/support.hpp` for endpoint configuration and graceful signal handling.
