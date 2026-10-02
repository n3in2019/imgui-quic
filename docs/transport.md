# Native draw transport

The host runs Dear ImGui and sends acknowledged draw snapshots through native
Google QUICHE to the browser. Resources and critical input use reliable streams;
small P frames and supersedable pointer updates use QUIC datagrams. Pointer
sequence fences preserve hover-before-button ordering. Epochs and resource
barriers prevent stale geometry from presenting after recovery or texture changes.

See the [endpoint guide](../tools/webtransport/README.md) for session framing,
authentication and deployment, and the [benchmark guide](../tools/benchmarks/README.md)
for reproducible loss, latency and bandwidth measurements.

### Exact draw frames

Use the matching bundled frontend with the native host.

An I frame contains complete geometry. A P frame copies a client-confirmed
baseline, resizes it, and applies changed byte ranges. The server chooses an I
frame when a delta is no smaller. Clients advertising motion support also accept
P frames containing per-list vertex translations followed by residual byte
patches. The residual restores exact original bytes, including float rounding,
clip changes and edits inside a moving window. The encoder chooses this form
only when it is smaller than the ordinary I/P packet. No entropy compressor is
used; bandwidth gains depend on the workload.

Bootstrap/recovery sends a single I frame until its rendered ACK. Outstanding
encoded frames have a 24 KiB budget, allowing one larger frame alone to make
progress. At most eight unacknowledged snapshots are retained per client, plus one
acknowledged baseline. The decoder retains nine snapshots. Each reconstructed
frame is limited to 16 MiB. Unsent frames may be replaced safely because P
frames only reference acknowledged baselines. A client stalled without an ACK
for ten seconds is disconnected (60 seconds before the first baseline ACK, to
allow texture initialization). Recovery requests resend textures and an I
frame; reconnect always starts with an I frame. No B frames or future-frame
buffering are used.

Textures use the existing reliable, ordered `0x02` channel and precede dependent
frames. Geometry changes and texture changes both trigger presentation. Host
font atlases and ordinary custom draw-list geometry are supported. Arbitrary
native renderer callbacks are not transported. Browser-local media stays local.

The shared server layout is scaled onto each browser canvas. This mode does not
provide independent UI state or local ImGui prediction; authoritative responses
still incur a network round trip. The bounded ACK window can limit throughput on
high-latency connections. The [native WebTransport endpoint](../tools/webtransport/README.md)
carries small P frames and mouse moves as datagrams, while critical inputs,
textures and large frames remain reliable. The browser uses WebTransport only;
connection failures retry it with bounded backoff. Unsupported browsers or missing
configuration show an error. QUIC/TLS runs inside the native application through Google QUICHE (C++) and BoringSSL.
Independent cancelable frame/resource streams remain future work.

Run `ctest --test-dir build --output-on-failure` and
`node tests/browser/draw_ip.mjs` after building with tests enabled. The latter
checks C++/JavaScript byte-exact reconstruction, byte-exact renderer dispatch and recovery. Native QUIC integration tests cover
sessions, recovery, ordering and reconnect.


### Images and video inside ImGui

Open the example's **Media** tab and choose **Sample image** or **Sample video**.
Images appear in **Server image** and video in **Images & video**, drawn
with `ImGui::Image`. Play/Pause, Restart, seeking, volume, mute and Clear are
ordinary ImGui controls. To use your own file, click **Open media file** in the
browser, drop a file on the page, or paste an image. Closing the ImGui media
window stops playback and releases its texture and object URL.

The browser decodes image/video files into a reserved WebGL texture; decoded
video frames redraw the latest ImGui draw lists even when draw data is
unchanged. Codec support depends on the browser. Video starts muted and paused;
playback errors are shown in the ImGui window. The generated color-bar samples
are included so no external media download is needed.

`include/imgui_quic_media.h` provides `imgui_quic_media_get()` and
`imgui_quic_media_control()` separately from the lifecycle header. Draw its
`IMGUI_QUIC_MEDIA_TEXTURE_ID` using `ImGui::Image`. Draw commands carry resolved 64-bit texture IDs. Each browser keeps
its own file and texture; the C++ controls address the currently active browser.
Files and decoded pixels are not uploaded or synchronized to other clients.

### Server-provided images

**Media → Sample image** displays pixels supplied by C++; **Update image**
replaces those pixels on every connected browser. The server retains the
latest texture so new browsers and reconnects receive the same image.

Use the separate `imgui_quic_texture.h` API after server initialization:

```cpp
uint64_t texture = imgui_quic_texture_upload_rgba(
    0, width, height, rgba_pixels, size_t(width) * height * 4);
// In your render callback:
ImGui::Image(ImTextureRef((ImTextureID)texture), ImVec2(width, height));
// Update in place by passing texture instead of 0 to the upload function.
```

The upload copies tightly packed RGBA8 pixels (top row first), returns zero on
failure, and limits each upload to 64 MiB. Texture IDs and stored pixels live
until server shutdown. Decode application image files to RGBA before upload.
This path is shared server content; **Open media file** remains an optional
browser-local preview. Video decoding/playback still runs in the browser.
