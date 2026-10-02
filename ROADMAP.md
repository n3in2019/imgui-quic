# ImGuiQuic roadmap

The current architecture runs Dear ImGui entirely on the native host and sends
acknowledged I/P draw data to a WebGL frontend. Window-motion deltas preserve
exact geometry bytes. Per-client ACK baselines, bounded outstanding snapshots,
reconnect recovery and cached texture delivery are implemented.

## Next work

- Measure input-to-visible latency under loss, jitter and bandwidth limits.
- Make the current fixed 24 KiB/eight-frame budget adaptive to link conditions.
- Separate native QUIC large frames/resources into cancelable streams. The
  in-process WebTransport endpoint and direct native state callbacks are implemented. See [transport research](docs/transport.md).
- Reduce per-client delta encoding work and benchmark larger custom draw lists.
- Improve multi-client feedback around the shared active-client layout.

These items are proposals, not implemented transport guarantees. Application
state, input hit testing, fonts and docking remain authoritative on the host.
