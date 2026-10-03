#!/usr/bin/env python3
"""Measure byte versus eight-byte patch scans with identical wire output."""
import argparse,hashlib,json,os,random,statistics,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
p=argparse.ArgumentParser();p.add_argument('--frames',type=int,default=900);p.add_argument('--trials',type=int,default=3)
p.add_argument('--output',default='docs/benchmarks/expanded/scan.json');a=p.parse_args()
cpu=min(os.sched_getaffinity(0));os.sched_setaffinity(0,{cpu})
scenes=['static','moving','dynamic','plots','tables','topology','dense']
variants={'byte':'byte','word':'build'}
jobs=[(t,s,v) for t in range(a.trials) for s in scenes for v in variants];random.Random(42).shuffle(jobs)
rows=[]
for trial,scene,variant in jobs:
    binary=ROOT/'build/comparison'/variants[variant]/'codec'
    row=json.loads(subprocess.check_output([str(binary),'imgui-quic-adaptive',scene,str(a.frames)],text=True))
    row.update(trial=trial,variant=variant);rows.append(row)
checks={}
for scene in scenes:
    streams=[subprocess.check_output([str(ROOT/'build/comparison'/folder/'codec'),'imgui-quic-adaptive',scene,'120','fixtures']) for folder in variants.values()]
    assert streams[0]==streams[1],scene
    checks[scene]=hashlib.sha256(streams[0]).hexdigest()
data=dict(frames=a.frames,trials=a.trials,cpu_affinity=cpu,fixture_sha256=checks,
          source_sha256={str(f.relative_to(ROOT)):hashlib.sha256(f.read_bytes()).hexdigest() for f in [ROOT/'src/draw_protocol.cpp',ROOT/'build/comparison/byte/draw_byte.cpp']},results=rows)
out=ROOT/a.output;out.parent.mkdir(parents=True,exist_ok=True);out.write_text(json.dumps(data,indent=2)+'\n')
for scene in scenes:
    values=[statistics.median(r['encode_mean_us'] for r in rows if r['scene']==scene and r['variant']==v) for v in variants]
    print(scene,*(round(v,3) for v in values),round((1-values[1]/values[0])*100,2),flush=True)
