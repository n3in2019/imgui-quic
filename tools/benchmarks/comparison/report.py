#!/usr/bin/env python3
"""Summarize same-run upstream and ImGuiQuic codec measurements."""
import json,statistics,sys
from pathlib import Path
source=Path(sys.argv[1]);target=Path(sys.argv[2]);data=json.loads(source.read_text());rows=data['results']
algs=data.get('algorithms',list(dict.fromkeys(r['algorithm'] for r in rows)))
lines=['# ImGuiQuic and upstream remoting encoders','',
'This comparison includes all variants in the same randomized run. It measures actual upstream encoding routines on the same Dear ImGui workload, not published numbers or separate demo applications.','',
f'Each case has {data["trials"]} trials, {data["warmup_frames"]} warmup frames and {data["frames"]} measured frames per trial. All encoders use the same Dear ImGui commit `{data["imgui_commit"]}`, compiler `{data["compiler"]}`, Release optimization and logical CPU {data["cpu_affinity"]}. The workload is 1280×720 with a 1000×650 window and 30 text rows. The runner randomizes case order with seed 42.','',
'The measured working tree contains uncommitted integration changes; raw metadata includes source hashes and dependency pins. The Git commit alone does not identify this encoder.', '',
'Rows use medians of per-trial statistics. Encode time includes serialization, encoding, related allocations and baseline updates, but not ImGui layout, socket I/O, client decoding or display. The mean-time range shows variation across trials. The 30 FPS rate is calculated draw-payload throughput; it is not measured TCP/UDP traffic.','']
for scene in ['moving','dynamic','static']:
 lines += [f'## {scene.capitalize()} scene','',
 '| Encoder | First UI frame B | Mean payload B/frame | Encode mean µs | Encode p95 µs | Trial mean range µs | Payload KiB/s at 30 FPS |',
 '|---|---:|---:|---:|---:|---:|---:|']
 for alg in algs:
  r=[x for x in rows if x['algorithm']==alg and x['scene']==scene]
  median=lambda k:statistics.median(x[k] for x in r)
  times=[x['encode_mean_us'] for x in r]
  lines.append(f'| {alg} | {median("first_bytes"):.0f} | {median("mean_bytes"):.1f} | {median("encode_mean_us"):.2f} | {median("encode_p95_us"):.2f} | {min(times):.2f}–{max(times):.2f} | {median("mean_bytes")*30/1024:.2f} |')
 lines += ['']
lines += ['## Interpretation and limits','',
'- `imgui-quic-adaptive` is the integrated, default-negotiated lossless encoder: block skipping, motion prediction and bounded XOR/LZ4 selection. All selection costs are timed. `imgui-quic` disables compression but retains the optimized exact scan.',
 f'- ImGuiQuic compression uses LZ4 `{data.get("lz4_commit","see source metadata")}`; RemoteImGui retains its own bundled LZ4. Its generated dependency changes symbol prefixes only to avoid linking collisions.',
'- imgui-ws uses its upstream default vertex-offset XOR/RLE encoder, adapted only for the Dear ImGui texture-ID accessor. Its diff size excludes incppect/WebSocket framing and uses 16-bit indices/32-bit texture IDs.',
'- netImgui uses the upstream conversion/compression functions and includes its command header. It quantizes positions to approximately 0.25-unit steps and UVs to u16, so it does not preserve the same geometry precision as the exact codec.',
'- RemoteImGui uses extracted upstream packing/difference/LZ4 code with counted socket writes, integer positions, fixed-point UVs and its older command schema. Its bundled LZ4 and chunk headers are included. The adapter submits each frame, without the old three-frame throttle, and emits a keyframe every 60 calls. Its old viewer and networking are not run.',
'- `raw` is canonical uncompressed serialization, not a remoting repository.',
'- All codecs assume availability of the previous frame. ACK delay, retransmission and backpressure are excluded. Static scenes force repeated encoder calls and do not model application-level suppression. First-frame bytes omit font textures and connection setup.',
'- These workloads test small text-heavy UIs, not large plots, images, many clients or every Dear ImGui feature. CPU turbo/governor and unrelated desktop activity are not controlled.',
'', '## Sources','']
for name,pin in data['upstream'].items():
 lines.append(f'- [{name} `{pin["commit"][:12]}`]({pin["url"].removesuffix(".git")}/tree/{pin["commit"]})')
lines += ['', '## Reproduce','', '```bash',
'python3 tools/benchmarks/comparison/run.py --frames 3000 --trials 7 --output docs/benchmarks/integrated-comparison/results.json',
'python3 tools/benchmarks/comparison/report.py docs/benchmarks/integrated-comparison/results.json docs/benchmarks/integrated-comparison/README.md','```','',
'- [Raw trials](results.json)',
'- [Validation results](validation.json): imgui-ws and netImgui round trips passed for all three scenes (1500 frames each), outside timing. RemoteImGui’s isolated LZ4 source matches upstream except symbol prefixes; its complete viewer round trip remains untested.',
'- [Build and adapter details](../../../tools/benchmarks/comparison/README.md)',
'- [ImGuiQuic decoder and actual network measurements](../integrated/README.md)',
'', 'Only ImGuiQuic has actual network and JavaScript decoder measurements in this experiment. Competing end-to-end traffic, CPU/RSS and input-to-display latency remain unmeasured; encoder results must not be presented as a full-product performance ranking.','']
target.parent.mkdir(parents=True,exist_ok=True);target.write_text('\n'.join(lines))
