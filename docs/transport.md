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
only when it is smaller than the ordinary I/P packet. Negotiated LZ4 blocks
further reduce larger updates as described below; gains depend on the workload.

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


### Lossless compressed geometry

Capability bit 7 enables raw LZ4 blocks. The browser requests this capability
when advertised by the server. Compression preserves the selected geometry
bytes; position/UV quantization is negotiated separately.

Compressed I `0x22`: frame ID u32, baseline zero u32, decoded length u32,
then one LZ4 block containing the entire geometry. Compressed P `0x23` has the
same header with an acknowledged baseline ID; the block contains the XOR of
current and baseline geometry bytes. Bytes beyond the baseline use zero;
shorter output truncates it. Length describes the reconstructed geometry,
not a nested packet or an LZ4 frame container. Integers are little-endian.

The sender first tries an exact patch/motion packet up to 512 bytes. Larger
updates try XOR/LZ4 (plain LZ4 for I frames), then a bounded exact candidate;
the smaller packet wins, with exact encoding winning ties. Incompressible
updates retain exact I/P fallback. Equal 32-byte spans bypass scalar scanning.

Compressed I frames are reliable and remain single-flight during bootstrap or
recovery. Compressed P frames use datagrams only when they fit the negotiated
limit; otherwise they use the reliable stream. Existing epochs, resource
barriers, presentation ACKs, eight-frame and 24 KiB windows still apply.
Decoders reject missing bases, truncated tokens, invalid offsets and output
beyond the declared length or 16 MiB maximum. Decode failure never mutates a
cached baseline and requests recovery through the existing ACK-zero path.

The C++ core builds pinned LZ4 1.10.0; the browser decodes bounded blocks in
JavaScript without another runtime asset. Run
`node tests/browser/compressed_draw.mjs` after changes to this path.

### Byte-lane compressed geometry

Capability bit 10 enables planar subtraction/LZ4, together with bit 7. I frame
`0x26` and P frame `0x27` have the usual 13-byte frame/base/decoded-length header,
then a u8 lane width (12 or 20) and one raw LZ4 block.

For each current byte, subtract the acknowledged baseline byte modulo 256;
use zero beyond the baseline or for an I frame. Concatenate residual bytes at
offsets `lane, lane+width, ...` for each lane in ascending order, then compress.
Decode LZ4 to the declared length, reverse the lane ordering, and add baseline
bytes modulo 256. Lane width must match the recovered geometry: 20 for `0x01`,
12 for `0x21`, `0x24` or `0x25`. Reject other widths/types and mismatches.

The sender tries this candidate only when the best ordinary packet is at least
4096 bytes. Up to 64 position/UV samples per list skip the second compressor for
stable attributes, such as color-only animation. This heuristic can miss size
savings; it never changes the recovered bytes. The compressor's destination is
bounded by the current winner, and the candidate must be strictly smaller.
All candidate selection and sampling costs belong in encoding benchmarks.

Planar I frames remain reliable/single-flight; P frames follow the same datagram
size limits, reliable fallback, resource barriers, epochs and presentation ACKs.
Declared output and expanded geometry remain bounded to 16 MiB. Decoding failure
must not mutate cached baselines. Run `node tests/browser/planar_draw.mjs`.

### Quantized geometry

Capability bits 6, 8 and 9 enable fine, quarter and integer position precision.
The browser requests integer precision by default, with fine/exact fallback for
older servers. `draw-precision=integer|quarter|fine|exact` selects the mode;
`draw-quantized=1|0` selects fine/exact for compatibility. If several precision
bits are supplied, the server prioritizes integer, then quarter, then fine.

The I/P envelope and acknowledged-baseline rules are unchanged. A packed decoded
payload begins with `0x21` (fine), `0x24` (quarter) or `0x25` (integer) instead of
`0x01`, followed by the same six float32
viewport values and u32 list count. Each list contains:

- Three u32 vertex/index/command counts.
- Two float32 origin coordinates (the first vertex position, zero for an empty list).
- Twelve-byte vertices: x/y as signed little-endian int16 offsets, divided by
  16/4/1 for fine/quarter/integer; u/v as u16 divided by 65535, and RGBA8 color.
- The canonical u32 indices and 36-byte draw commands.

The sender rounds to the nearest quantization step. Position offsets outside
[-32768, 32767], UVs outside [0, 1], nonfinite inputs or reconstructed position
error above half the selected step cause the entire frame to use canonical float
geometry. Packing is also skipped unless it reduces the full geometry size.

P patches operate on packed bytes. The receiver retains those bytes as its ACK
baseline and expands a separate canonical float buffer for validation/rendering.
Motion prediction (`0x0f`) applies only to canonical float baselines; packed
frames use ordinary patches, including changes to their list origins. Switching
between packed and exact fallback frames is valid across an acknowledged P base.
Expanded geometry is bounded to 16 MiB before allocation. Texture prerequisites,
presentation-before-ACK, recovery and nine-frame cache limits are unchanged.

`tests/browser/quantized_draw.mjs` checks cross-language error bounds, unchanged
metadata, format transitions, malformed geometry and explicit negotiation.
Run interoperability with `IMGUI_QUIC_TEST_PRECISION=integer|quarter|fine|exact`
to choose the independent test client's mode. `IMGUI_QUIC_TEST_QUANTIZED=1` retains
fine-mode compatibility. `IMGUI_QUIC_TEST_PLANAR=0` disables planar negotiation
for fallback tests. These environment variables are for test tooling.

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
