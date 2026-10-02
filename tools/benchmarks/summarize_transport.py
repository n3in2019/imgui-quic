#!/usr/bin/env python3
"""Summarize repeated transport trials; preserve failures and expose ranges."""
import argparse
import json
from pathlib import Path
import statistics

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('results',nargs='?',default='build/benchmarks/transport/results.json')
args=parser.parse_args()
source=Path(args.results)
rows=json.loads(source.read_text())
profiles=list(dict.fromkeys(r['profile'] for r in rows))
scenes=list(dict.fromkeys(r['scene'] for r in rows))
metrics=['fps','frame_age_p95_ms','display_age_p95_ms','max_gap_ms','input_p95_ms','wire_KiB_s','server_cpu_pct','server_rss_MiB','server_context_switches_s']
backend='quiche'
wt_label='QUICHE'
seconds=sorted({r.get('seconds',8) for r in rows})
trials=len({r['trial'] for r in rows})
summary=[]
def span(group,key):
    values=[r[key] for r in group if r.get(key) is not None]
    if not values:return '—'
    return f'{min(values):.1f}–{max(values):.1f}' if len(values)>1 else f'{values[0]:.1f}'
lines=[f'# {wt_label} WebTransport benchmark', '',
    '30 FPS; Release core; real Linux netem packet loss in an isolated network namespace.',
    f'Each trial measures {", ".join(f"{n:g}" for n in seconds)} seconds after the first frame plus 1 second warmup. {trials} trial orders/seeds.',
    'Reported intervals are min–max across trials, not confidence intervals or pooled percentiles.',
    'Age is generation-to-client decode/ACK, excluding browser WebGL/compositor. High-load startup backlog may remain.',
    'Impaired profiles share a 2 Mbit/s rate budget across both directions; local is unlimited.', '',
    '| Network | Scene | Path | FPS | Frame age p95 ms | Sampled displayed age p95 ms | Longest gap ms | Wire KiB/s | Failures |',
    '|---|---|---|---:|---:|---:|---:|---:|---:|']
for profile in profiles:
    for scene in scenes:
        for transport in ('WT',):
            group=[r for r in rows if r['profile']==profile and r['scene']==scene and r['transport']==transport]
            good=[r for r in group if 'error' not in r]
            if not group:continue
            entry=dict(backend=backend,profile=profile,scene=scene,transport=transport,failures=len(group)-len(good),trials=len(group))
            for key in metrics:
                values=[r[key] for r in good if r.get(key) is not None]
                entry[key]=dict(mean=statistics.mean(values),min=min(values),max=max(values)) if values else None
            summary.append(entry)
            lines.append(f'| {profile} | {scene} | {wt_label if transport == "WT" else transport} | '+ ' | '.join(span(good,key) for key in
                ['fps','frame_age_p95_ms','display_age_p95_ms','max_gap_ms','wire_KiB_s'])+f' | {entry["failures"]}/{len(group)} |')
lines += ['', '## Pointer echo delivery', '',
    'Input latency alone excludes skipped updates. The following includes delivery counts.', '',
    '| Network | Scene | Path | Trial | Observed/sent | Input p95 ms |',
    '|---|---|---|---:|---:|---:|']
for r in rows:
    if 'error' not in r:
        lines.append(f'| {r["profile"]} | {r["scene"]} | {r["transport"]} | {r["trial"]} | '
                     f'{r["input_seen"]}/{r["input_sent"]} | {r["input_p95_ms"]} |')
lines += ['', '## Native server resource use', '',
    'CPU is percent of one logical CPU during measurement; RSS is resident memory at the end, not peak.',
    'Includes ImGui, draw encoding, HTTP and native transport. Python client CPU is excluded.', '',
    '| Network | Scene | Path | Server CPU % | Server RSS MiB | Context switches/s |',
    '|---|---|---|---:|---:|---:|']
for profile in profiles:
    for scene in scenes:
        for transport in ('WT',):
            group=[r for r in rows if r['profile']==profile and r['scene']==scene and r['transport']==transport and 'error' not in r]
            lines.append(f'| {profile} | {scene} | {wt_label if transport == "WT" else transport} | {span(group,"server_cpu_pct")} | {span(group,"server_rss_MiB")} | {span(group,"server_context_switches_s")} |')
lines += ['', '## Limits', '',
    '- Native Release host with in-process Google QUICHE and an independent Python draw decoder.',
    '- No browser presentation latency or hardware GPU time was measured.',
    '- First-frame time is excluded from successful-case metrics. Timeout rows are retained as failures.',
    '- netem bytes include both directions and retransmissions leaving the queue, not physical Ethernet framing.',
    '- Dynamic content exceeds the 2 Mbit/s budget at the target 30 FPS even before retransmission overhead.',
    '- Pointer updates are periodic at 10 Hz; local input latency differences can reflect phase relative to the 30 Hz render loop.',
    '- Short exploratory trials do not establish a production SLA. No host network settings were changed.', '']
source.with_name('summary.json').write_text(json.dumps(summary,indent=2)+'\n')
source.with_name('report.md').write_text('\n'.join(lines))
print('\n'.join(lines[:lines.index('## Pointer echo delivery')]))
