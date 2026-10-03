#!/usr/bin/env python3
"""Summarize production integer/planar encoding, decoding and native QUIC."""
import json,statistics
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3];OUT=ROOT/'docs/benchmarks/packed-live'
read=lambda name:json.loads((OUT/name).read_text())
def med(rows,key):
    values=[r[key] for r in rows if r.get(key) is not None]
    return statistics.median(values) if values else None
def fmt(v):return '—' if v is None else f'{v:.2f}'
data=read('results.json');rows=data['results']
lines=['# Integer geometry and planar compression','',
'The browser defaults to integer position offsets, u16 UVs and negotiated planar subtraction/LZ4. Small patches retain the fast path. Large changing attributes can use lane-transposed residuals; stable attribute samples skip the second compressor. These formats run in the native server and production browser.','',
'Nearest integer offsets relative to each list’s original float origin bound coordinate error to half a logical unit. UV error is approximately 1/131070 plus float reconstruction rounding. Colors, indices, clips, textures, offsets and input coordinates remain unchanged. Quarter, fine and exact modes are selectable. Older clients/servers negotiate their supported formats.','',
'## Same-run encoding comparison','',
f'Five randomized trials per case use {data["frames"]} measured frames and 30 warmup frames, logical CPU {data["cpu_affinity"]}, the same pinned Dear ImGui and Release compiler. Timings include serialization, quantization, allocation, ordinary/planar candidate evaluation, sampling, selection and baseline updates; they exclude layout/network/decoding. Figures are medians of per-trial statistics. Source hashes identify the dirty working tree in [raw results](results.json).','',
'`imgui-quic-adaptive` disables planar/quantization. `imgui-quic-adaptive-live` enables planar with float geometry. `imgui-quic-q1-adaptive-live` measures the integer/planar browser default. Production selection uses frame bytes, a 4096-byte threshold and up to 64 position/UV samples per list, never benchmark-scene names.','']
algs=['imgui-quic-adaptive','imgui-quic-adaptive-live','imgui-quic-q1-adaptive-live','RemoteImGui','netImgui']
for key,label in [('mean_bytes','B/frame'),('encode_mean_us','Encode mean µs')]:
 lines += [f'### {label}','',
 '| Scene | Exact XOR/LZ4 | Exact + planar | Integer + planar | RemoteImGui | netImgui |',
 '|---|---:|---:|---:|---:|---:|']
 for scene in data['scenes']:
  lines.append(f'| {scene} | '+' | '.join(fmt(med([r for r in rows if r['scene']==scene and r['algorithm']==alg],key)) for alg in algs)+' |')
 lines+=['']
lines+=['The default has lower mean payload than netImgui and RemoteImGui in all seven measured scenes. imgui-ws is one byte smaller for forced static encoder calls (12 versus 13 bytes); production suppresses identical geometry apart from heartbeat/resource changes. Full imgui-ws results, first-frame/p95 sizes and timing variation remain in the raw file. This is a payload result, not a universal performance ranking.','',
'Encoding still costs more than netImgui. Planar evaluation increases curve/table/topology CPU work; packing unchanged/moving text costs more than the exact path. Attribute sampling prevents the dense-color workload’s rejected second compression, but can miss size savings. Compression preserves selected packed bytes exactly; packing intentionally changes floats.','',
'netImgui uses approximately 0.25-unit position precision and u16 UVs. RemoteImGui truncates positions to integers, packs fixed-point UVs and uses an older command schema. ImGuiQuic rounds relative offsets to nearest integers while retaining float list origins, so integer geometry is not identical between implementations. Upstream adapter/header differences, throttle adaptations and omitted texture bootstrap are documented in [the guide](../../../tools/benchmarks/comparison/README.md). All codecs assume immediate previous-frame availability here.','',
'## Production JavaScript decoding','',
'Five Node/V8 trials use 30 warmup and 120 measured frames from the actual C++ encoder. The timer includes decoding, LZ4, unshuffle/addition, quantized expansion, validation and cache commit. Error-bound and unchanged-metadata checks run outside timing. WebGL/compositor are excluded. Case order is fixed; GC/desktop activity are uncontrolled. These are not browser presentation timings.','',
'| Scene | Exact XOR/LZ4 mean/p95 µs | Exact + planar mean/p95 µs | Integer + planar mean/p95 µs |',
'|---|---:|---:|---:|']
decoded=read('decode.json')['results']
for scene in ['moving','dynamic','plots','tables','topology','dense']:
 values=[]
 for alg in algs[:3]:
  cases=[r for r in decoded if r['scene']==scene and r['algorithm']==alg]
  values.append(f'{med(cases,"decode_validate_commit_mean_us"):.0f}/{med(cases,"decode_validate_commit_p95_us"):.0f}')
 lines.append(f'| {scene} | '+' | '.join(values)+' |')
lines+=['','[Raw decode trials](decode.json) include cache/expanded bytes and position/UV error. Nine packed or float baselines remain bounded separately from the expanded frame. The largest mean default decode is about 14 ms. Device-specific browser and ARM timings remain unmeasured.','',
'## Actual native QUIC comparison','',
'Two randomized six-second trials per mode/scene/profile use the same native host and independent Python receivers. `exact-xor` requests exact geometry with planar disabled; `integer-planar` requests integer packing and planar. All sixteen cases passed. The impaired profile uses 100 ms configured RTT, 10 ms jitter, 5% loss and 2 Mbit/s in an isolated loopback namespace with MTU 1500/offloads disabled. Setup/font bootstrap is excluded.','',
'| Scene | Profile | Mode | FPS | Wire KiB/s | Frame age p95 ms | Input p95 ms | Inputs observed/sent, both trials |',
'|---|---|---|---:|---:|---:|---:|---:|']
network=read('network.json');assert len(network)==16 and not any('error' in r for r in network)
for scene in ['plots','tables']:
 for profile in ['local','100ms-5pct']:
  for mode in ['exact-xor','integer-planar']:
   cases=[r for r in network if r['scene']==scene and r['profile']==profile and r['mode']==mode]
   values=[fmt(med(cases,k)) for k in ['fps','wire_KiB_s','frame_age_p95_ms','input_p95_ms']]
   lines.append(f'| {scene} | {profile} | {mode} | '+' | '.join(values)+f' | {sum(r["input_seen"] for r in cases)}/{sum(r["input_sent"] for r in cases)} |')
lines+=['','Weak-network freshness and received FPS improve. Local curves have lower FPS/higher age with the Python planar receiver: per-byte unshuffle/expand loops take roughly 40 ms per frame. This receiver bottleneck is retained in the report. The separate production JavaScript curve decoder measures about 5 ms; that difference does not establish browser end-to-end FPS.','',
'Wire rates include QUIC overhead/retransmissions; delivering more frames can increase throughput despite smaller frames. Age ends at decoded timestamp/consumed-pointer markers, not GPU scanout. Input percentiles include observed inputs only. Large P frames still use reliable delivery and can stall under loss. Independent cancelable frame streams are not implemented. Short two-trial tail estimates vary. [Raw network trials](network.json) include server CPU/RSS, packet types, losses and receiver costs.','',
'## Validation','',
'[Validation records](validation.json) cover six CTests, frontend suites, 450 quantized error-bound fixtures, 240 planar cross-language fixtures, 12/20 lanes, aged bases, precision/size transitions, malformed LZ4/stride rejection, presentation-before-ACK and recovery. Native QUIC checks cover integer, quarter, fine and legacy exact modes. The demo rendered in the in-app browser without console warnings/errors. This does not establish every DPI/font or ARM performance.','',
'## Reproduce','',
'Use the pins and build setup in [the guide](../../../tools/benchmarks/comparison/README.md).','',
'```bash',
'python3 tools/benchmarks/comparison/run.py --frames 900 --trials 5 --scenes static moving dynamic plots tables topology dense --algorithms imgui-quic-adaptive imgui-quic-adaptive-live imgui-quic-q1-adaptive-live imgui-ws netImgui RemoteImGui --output docs/benchmarks/packed-live/results.json',
'node tools/benchmarks/comparison/decode.mjs docs/benchmarks/packed-live/decode.json --packed-live',
'IMGW_BENCH_HOST_NETNS="$(readlink /proc/self/ns/net)" unshare --user --map-root-user --net build/webtransport_venv/bin/python tools/benchmarks/comparison/packed_network.py --seconds 6 --trials 2 --output docs/benchmarks/packed-live/network.json',
'python3 tools/benchmarks/comparison/packed_report.py','```','',
'All timings are x86-64. Compiler, source/dependency hashes and scenes are in raw encoding metadata. CPU turbo/governor and unrelated desktop work were not controlled; assess trial variability rather than small timing differences.','']
(OUT/'README.md').write_text('\n'.join(lines))
