#!/usr/bin/env python3
"""Generate expanded codec, live-network and research evaluation tables."""
import json,statistics
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3];OUT=ROOT/'docs/benchmarks/expanded'
read=lambda name:json.loads((OUT/name).read_text())
def median(rows,key):
    values=[r[key] for r in rows if r.get(key) is not None]
    return statistics.median(values) if values else None
def fmt(value):return '—' if value is None else f'{value:.2f}'
data=read('results.json');lines=['# Expanded remote ImGui evaluation','',
'This report evaluates the earlier source snapshots identified by its raw hashes. The current production integer/planar codec is evaluated in [the production comparison](../packed-live/README.md).','',
'Seven shared workloads compare native production encoding with pinned upstream routines. Separate tests compare the actual QUICHE and imgui-ws network stacks, then evaluate additional lossless codec candidates. There is no single winner across payload size, encoding cost, precision and latency.','',
'## Encoder corpus','',
f'The corpus uses {data["trials"]} randomized trials, {data["frames"]} measured frames and 30 warmup frames, pinned to logical CPU {data["cpu_affinity"]}. The simulation advances at 30 FPS without pacing. Encoding includes serialization, allocations, codec selection and baseline ownership updates; layout, networking and decoding are excluded. Figures are medians of per-trial means. The working tree is dirty; dependency pins and source hashes are recorded in [raw results](results.json). These measurements precede the eight-byte patch-scan change below.','',
'Scenes use a 1280×720 viewport and a 1000×650 window: stationary text, moving text, changing text, a 4096-point antialiased curve, a 32-row/8-column table, variable triangle counts, and 6000 colored triangles. Vertex counts remain within the older adapters’ 16-bit index limits. Textures and bootstrap are excluded.','',
'| Scene | Encoder | Mean B/frame | Encode mean µs | Encode p95 µs |','|---|---|---:|---:|---:|']
for scene in data['scenes']:
 for algorithm in data['algorithms']:
    rows=[r for r in data['results'] if r['scene']==scene and r['algorithm']==algorithm]
    lines.append(f'| {scene} | {algorithm} | {median(rows,"mean_bytes"):.1f} | {fmt(median(rows,"encode_mean_us"))} | {fmt(median(rows,"encode_p95_us"))} |')
lines+=['','### Fidelity and adapter limits','',
'ImGuiQuic preserves canonical float32 positions/UVs and u32 indices. netImgui packs positions at approximately 0.25-unit resolution and UVs as u16; RemoteImGui truncates positions to integers and packs fixed-point UVs. Their smaller payloads are not equal-precision comparisons. imgui-ws normalizes positions by subtracting and later restoring an origin in floating point, which can also change fractional bits.','',
'netImgui’s word-oriented changed/unchanged runs and packed contiguous buffers give it the lowest encoding cost among the remoting codecs in this corpus. RemoteImGui’s byte subtraction, LZ4, 12-byte vertices and integer positions favor smooth curves and tables. ImGuiQuic’s motion prediction favors window translation; its XOR/LZ4 path favors changing vertex colors. Raw serialization has no compression work and is a cost floor, not a competing transport.','',
'RemoteImGui’s extracted adapter uses its bundled LZ4, submits every supplied frame and emits a keyframe every 60 calls. Its old application throttle and viewer are not run. imgui-ws’s codec figures omit incppect/WebSocket metadata; netImgui includes command headers. Every encoder here has an immediately available previous frame. These codec figures exclude ACK delay and congestion.','',
'## Actual network stacks','',
'Each profile has two randomized trials of eight measured seconds for each scene and stack. Impaired profiles use a 2 Mbit/s path, configured RTT/jitter/loss in an isolated loopback namespace, with MTU 1500 and offloads disabled. Both hosts generate the shared scene plus timestamp/input markers at 30 FPS. The independent receivers decode and measure generation-to-marker-decode age and pointer-input-to-marker-decode latency. This is not browser/GPU presentation latency. Input percentiles include observed inputs only; the count column exposes missing observations.','',
'QUIC uses TLS, acknowledgements and production frame flow control. imgui-ws runs its actual incppect/uWebSockets/uSockets server without TLS; its receiver subscribes to background/UI variables and polls at 30 Hz instead of the upstream browser’s default 20 Hz. Variable publication is not an atomic whole-frame snapshot. Receiver CPU, font bootstrap and browser rendering are excluded. Wire rates include transport overhead/retransmission counted by the namespace filters. Server CPU measures all process threads; 100% represents one logical core.','',
'| Scene | Profile | Stack | FPS | Wire KiB/s | Marker age p95 ms | Input p95 ms | Inputs seen/sent, both trials | CPU % | RSS MiB |','|---|---|---|---:|---:|---:|---:|---:|---:|---:|']
network=read('network.json');assert len(network)==32 and not any('error' in r for r in network)
for scene in ['moving','dynamic']:
 for profile in ['local','100ms-1pct','300ms-3pct','100ms-5pct']:
  for stack in ['imgui-quic','imgui-ws']:
    rows=[r for r in network if r['scene']==scene and r['profile']==profile and r['stack']==stack]
    seen=sum(r['input_seen'] for r in rows);sent=sum(r['input_sent'] for r in rows)
    values=[fmt(median(rows,k)) for k in ['fps','wire_KiB_s','frame_age_p95_ms','input_p95_ms']]
    lines.append(f'| {scene} | {profile} | {stack} | '+ ' | '.join(values)+f' | {seen}/{sent} | {fmt(median(rows,"server_cpu_pct"))} | {fmt(median(rows,"server_rss_MiB"))} |')
lines+=['','[Raw network trials](network.json) include display staleness, input observation counts, losses, packet types and receiver handle times. Handle times are Python implementation costs and should not be ranked as browser decoder speeds. Only two short trials were run: tail results vary considerably, particularly when reliable delivery accumulates a backlog. netImgui and RemoteImGui network stacks remain unmeasured.','',
'## Client fanout','',
'Three local trials per client count, six measured seconds each, use independent QUIC decode/ACK receivers and the dynamic scene. The benchmark alone admits eight clients from the same address. Server CPU/RSS excludes Python receiver processes. All clients observe about 30 FPS. This does not establish many-client behavior under loss.','',
'| Clients | Server CPU % | Server RSS MiB | Lowest per-client FPS across trials |','|---:|---:|---:|---:|']
fanout=read('fanout.json');assert len(fanout)==9
for count in [1,4,8]:
 rows=[r for r in fanout if r['clients']==count]
 lines.append(f'| {count} | {fmt(median(rows,"server_cpu_pct"))} | {fmt(median(rows,"server_rss_MiB"))} | {min(v for r in rows for v in r["client_fps"]):.2f} |')
lines+=['','[Raw fanout trials](fanout.json).','',
'## Integrated patch-scan optimization','',
'The production scanner inspects eight bytes together when constructing changed ranges, then trims the equal suffix. Patch boundaries and every wire byte match the prior byte-at-a-time scan. This reduces loop work without introducing a new protocol type or changing precision. Three randomized 900-frame trials per variant compare the complete adaptive encoder, including serialization. The reference is generated from the same source with only this optimization removed.','',
'| Scene | Byte scan µs | Eight-byte scan µs | Mean encode reduction |','|---|---:|---:|---:|']
scan=read('scan.json')
for scene in data['scenes']:
 rows=[r for r in scan['results'] if r['scene']==scene]
 old=median([r for r in rows if r['variant']=='byte'],'encode_mean_us');new=median([r for r in rows if r['variant']=='word'],'encode_mean_us')
 lines.append(f'| {scene} | {old:.2f} | {new:.2f} | {(1-new/old)*100:.1f}% |')
lines+=['','[Raw scan trials](scan.json) also record identical complete wire fixtures for 120 measured frames plus warmup in each scene. The native randomized scanner test compares 2000 grow/shrink/sparse-difference cases against an independent scalar implementation.','',
'## Further lossless codec research','',
'The [research trials](research.json) compare candidates and upstream codecs in one randomized run: three 900-frame trials per case. Candidate timing includes canonical serialization, transform, compression and baseline updates. It excludes a future adaptive selector, browser decode and networking. The estimated 13-byte envelope is not a supported wire packet. These candidates are not enabled in interactive sessions.','',
'| Scene | Encoder / candidate | B/frame | Encode mean µs |','|---|---|---:|---:|']
research=read('research.json')
for scene in research['scenes']:
 for algorithm in research['algorithms']:
    rows=[r for r in research['results'] if r['scene']==scene and r['algorithm']==algorithm]
    lines.append(f'| {scene} | {algorithm} | {median(rows,"mean_bytes"):.1f} | {fmt(median(rows,"encode_mean_us"))} |')
lines+=['','`plane20-sub` takes a modulo-256 byte difference from the previous canonical frame, transposes it into 20 byte lanes and applies LZ4. `meshopt20` pads whole canonical XOR residuals into 20-byte groups and uses meshoptimizer’s lossless vertex codec. Neither implementation yet separates actual vertex/index/command ranges; they are applicability probes rather than optimized semantic encoders. Verification decompresses, reverses the transform and checks original canonical bytes outside timing.','',
'The [4/20-lane experiments](experiments.json), [XOR/subtraction comparison](residual-experiments.json) and [4/20-byte meshoptimizer experiments](meshopt-experiments.json) use shorter three-trial, 300-frame runs. Their raw results remain separate from the final research comparison. meshoptimizer is pinned to `9e1f07b159d3cb777f1c67ed31fc11fd117986f4` (v1.3); it is an optional benchmark dependency.','',
'### External research and applicability','',
'- [meshoptimizer](https://github.com/zeux/meshoptimizer/tree/9e1f07b159d3cb777f1c67ed31fc11fd117986f4) provides a lossless vertex codec with internal byte deinterleaving and optimized decoders. Its documentation recommends packed, quantized input; codec losslessness does not require applying those lossy preprocessing steps. For ImGui, preserve vertex/triangle order, draw-command offsets and alpha blending. A per-list attribute/index integration is more representative than grouping a whole frame.','',
'- [Bitshuffle](https://github.com/kiyo-masui/bitshuffle) reorganizes typed bits to expose redundancy before LZ4, using SIMD on supported machines. This motivates block-local attribute shuffling rather than repeatedly transposing a whole mixed frame. Its published throughput is not an ImGui benchmark; an ARM and browser/Wasm implementation must be measured separately.','',
'- [Chimp](https://vldb.org/pvldb/vol15/p3058-liakos.pdf) studies lossless floating-point XOR coding and the cost of leading/trailing-zero metadata. It motivates specialized coordinate residuals. ImGui vertex ordering and changing topology differ from a stable time series; coordinate identity must be established before using temporal prediction.','',
'- [Stream VByte](https://arxiv.org/abs/1709.08990) separates integer control bytes from payloads to support SIMD decoding. It is relevant to index deltas and patch offsets/counts. Its paper’s throughput is not a measurement of this project, and extra per-frame metadata can lose on tiny patches.','',
'### Adaptive frame selection','',
'The current encoder already chooses ordinary I/P, motion P and XOR/LZ4 I/P using the acknowledged baseline. A useful extension is a negotiated per-list or per-block codec, while retaining the same frame ID, baseline ID, resource barriers and presentation ACK. Candidate IDs must be distinct from existing compressed-frame IDs; older clients keep the current encoding.','',
'Select from data statistics, not application names or benchmark scenes:','',
'1. Suppress unchanged geometry; use motion P for coherent translations and patch P for small edits.','2. For broad changes, sample changed-byte density, zero residual runs and stable vertex/index/command ranges. Bound sample work and retain a fast path for small packets.','3. Evaluate attribute-shuffled subtraction/LZ4 for coordinate-heavy changes, and specialized color/index coding where samples justify it. Topology changes require per-list ranges so one resized list does not misalign all later residuals.','4. Use a latency/CPU budget and measured network throughput. A decision model can estimate encode cost + decode cost + bytes/available throughput, with backlog/deadline penalties. This is a design hypothesis, not a validated runtime selector. Do not run every full encoder and ignore selection costs in benchmarks.','5. Use hysteresis, periodic bounded re-evaluation and exact fallback. Keep every referenced baseline acknowledged; new codec representations must reconstruct the canonical bytes before cache commit and presentation ACK.','',
'Required acceptance checks for an extension include bounded malformed-input handling, aged ACK bases, grow/shrink, quantized/exact transitions, epoch/recovery, bootstrap single-flight, loss and browser decoding. Whole-frame savings can disappear once fragmentation, retransmission and decoder cost are included. The evidence supports multiple encoding choices; it does not establish a universal best codec.','',
'## Validation and reproduction','',
'See [validation](validation.json) for completed checks and [the benchmark guide](../../../tools/benchmarks/comparison/README.md) for dependencies, pinned checkouts, adapters and network commands. CPU turbo/governor and unrelated desktop workloads were not controlled. All performance measurements are local x86-64; no ARM timing claim is made.','',
'```bash','python3 tools/benchmarks/comparison/run.py --frames 900 --trials 5 --scenes static moving dynamic plots tables topology dense --algorithms imgui-quic-adaptive imgui-ws netImgui RemoteImGui raw --output docs/benchmarks/expanded/results.json',
'python3 tools/benchmarks/comparison/scan.py',
'python3 tools/benchmarks/comparison/run.py --frames 900 --trials 3 --scenes moving dynamic plots tables topology dense --algorithms imgui-quic-adaptive imgui-quic-plane20-sub imgui-quic-meshopt20 netImgui RemoteImGui --output docs/benchmarks/expanded/research.json',
'python3 tools/benchmarks/comparison/expanded_report.py','```','',
'Re-running the first command after the scan optimization produces the newer encoder, not the archived pre-optimization source hashes. Preserve original reports when evaluating later changes.','',
'## Upstream references','']
for name,pin in data['upstream'].items():lines.append(f'- [{name} `{pin["commit"][:12]}`]({pin["url"].removesuffix(".git")}/tree/{pin["commit"]})')
(OUT/'README.md').write_text('\n'.join(lines)+'\n')
