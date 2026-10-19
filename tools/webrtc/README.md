# HTTP + WebRTC (no HTTPS setup)

An optional native C++ backend using libdatachannel. The page is served over
plain HTTP, SDP is exchanged over plain WebSocket, and interactive traffic uses
WebRTC DataChannels (SCTP/DTLS/ICE/UDP). No HTTPS certificate files, CA, certificate
pin or browser security exceptions are required. DTLS still encrypts data and
verifies the fingerprint exchanged in SDP; it is not raw unencrypted UDP.

This first version targets a trusted LAN. HTTP and WS do not authenticate the
server or protect the page, signaling or bearer token against an active network
attacker. Keep it on a trusted network; use an authenticated secure deployment
for untrusted networks. There is no external signaling service, STUN or TURN
configured, and NAT traversal across unrelated networks is not promised.

## Build and start (Linux x86-64 / AArch64)

Requires Git, CMake 3.21+, a C/C++17 compiler, OpenSSL development files and Node
22+ for tests. The optional dependency is libdatachannel 0.24.1, commit
`a02b751917ac8afc8c58dc6f4461d25ff9465d48`, with its exact submodule revisions.
QUICHE, Bazel, Clang and ICU are not needed for this configuration.

From the repository root:

```bash
bash tools/webrtc/build-dependency.sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DIMGUI_QUIC_NATIVE_QUIC=OFF -DIMGUI_QUIC_WEBRTC=ON \
  -DCMAKE_PREFIX_PATH="$PWD/build/rtc-prefix" \
  -DIMGUI_QUIC_BUILD_EXAMPLES=ON -DIMGUI_QUIC_BUILD_TESTS=ON
cmake --build build -j4
ctest --test-dir build --output-on-failure --no-tests=error
./build/examples/imgui_quic_dev --transport webrtc --example minimal \
  --address 127.0.0.1 --port 8888 --signal-port 8889 \
  --credentials build/webrtc_dev
```

Open the complete URL from `build/webrtc_dev/url.txt`. It has an `http://` page
URL and a fragment containing `rtc-url=ws://.../signal` and a random bearer token.
The token is not in an HTTP request path or WebSocket URL. `rtc-token` and URL
files are created in a private directory; do not publish them. No `cert.pem` or
`key.pem` is created in this mode. Copy the complete generated link to the browser.

For another machine on the LAN, replace `--address 127.0.0.1` with the server's
actual LAN IPv4 address. Allow the HTTP and signaling TCP ports and ICE UDP
traffic through the local firewall (ICE currently selects ephemeral UDP ports).
Do not advertise `0.0.0.0`: the launcher uses a local loopback URL for that bind
address. `--executable path` can supervise your own application instead of an
example. The backend is independent of the launcher's HTTP file server.

The existing WebTransport launcher mode is unchanged and remains the default.
Choose explicitly with `--transport webrtc`; a failed WebTransport attempt never
silently downgrades to HTTP/WebRTC. Use a fresh build directory when comparing
configurations. The new backend is OFF by default, so existing users do not gain
a dependency on libdatachannel.

## Application API

```cpp
#include "imgui_quic_webrtc.h"
// After imgui_quic_init(), before clients; do not start WebTransport as well.
imgui_quic_webrtc_config_t rtc{};
rtc.host = "192.168.1.50";
rtc.signaling_port = 8889;
rtc.token_file = "private/rtc-token";
if (imgui_quic_start_webrtc(&rtc) != 0) { /* startup failed */ }
// imgui_quic_shutdown() stops listeners, closes peers and joins the worker.
```

The example launcher uses `IMGUI_QUIC_WEBRTC=1`, `IMGUI_QUIC_HOST`,
`IMGUI_QUIC_PORT` and `IMGUI_QUIC_TOKEN_FILE`. An existing `IMGUI_QUIC_CERT` must
not be set when using the WebRTC example path. Public lifecycle, media, texture
and native ImGui APIs stay unchanged. Configuration strings are copied.

## Transport contract

- `imgui-reliable`: ordered, fully reliable. Length-prefixed records are split
  into SCTP messages no larger than 16 KiB. This supports textures larger than the
  negotiated SCTP message limit without changing draw or texture wire bytes.
- `imgui-unreliable`: unordered, `maxRetransmits=0`. A message holds one existing
  transport envelope. P frames up to 1100 bytes and latest pointer motion use it;
  larger frames and all I frames use the reliable channel. Both channels share
  one SCTP association and congestion control; this does not claim full isolation.
- The existing `MixedReceiver` and `MixedSocket.send` implement resource barriers,
  epoch checks, monotonic presentation, pointer fences and input batches. Decoder,
  WebGL renderer and submit-before-commit-before-ACK ordering are shared.
- Control resources must be acknowledged before dependent frames are polled.
  A sent-frame window of eight and core ACKed-baseline rules remain enforced.
  ACKs for unsent frames are ignored. Recovery changes epoch and requests an I
  frame; stalled unreliable-only output triggers recovery after two seconds.
- Default admission: eight sessions total, two per IPv4 address, including
  unauthenticated signaling sessions. Authentication timeout is five seconds and
  initial negotiation timeout is twenty seconds. SDP is bounded to 64 KiB/depth
  eight. Incoming application records are bounded to 1 MiB (including envelope),
  while outgoing textures retain the existing 64 MiB texture limit.
- Callbacks enqueue bounded events with weak ownership. One worker mutates native
  state and transport sequencing. Mailboxes only retain a shared wake object;
  the worker has a five-millisecond fallback poll, not a real-time guarantee.
- No media capture is requested. HTTP clipboard APIs and other secure-context
  features remain subject to browser restrictions; DataChannel does not remove
  those restrictions.

## Validation

```bash
node tests/browser/webrtc.mjs
# Real Chromium; installs browser binaries and OS libraries on a suitable machine:
npm install --prefix build/browser --save-exact playwright@1.62.1
node build/browser/node_modules/playwright/cli.js install --with-deps chromium
node tests/browser/webrtc_e2e.mjs
```

The real test loads `http://imgui-http.test:18888` with a test-only DNS mapping to
loopback and asserts `isSecureContext === false`. It uses production HTTP serving,
WebSocket signaling, native WebRTC, draw decoder and WebGL. It checks both channel
paths, a 128 KiB texture, native button input, forged ACK rejection, explicit
recovery, invalid token rejection and reload/reconnect. Ports 18888/18889 must be
free. JSON, native logs and a screenshot are saved under `build/webrtc-acceptance`;
any failure returns exit code 1. No HTTPS or secure-origin override is used.
`IMGUI_WEBRTC_TEST_CHROMIUM` may select a browser executable. The explicit
`IMGUI_WEBRTC_TEST_NO_SANDBOX=1` is for isolated root containers only; CI retains
sandboxing. A successful software-rendered test is functional acceptance only,
not RK3588 GPU, latency or physical-display performance acceptance.
