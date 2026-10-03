#!/usr/bin/env python3
"""Benchmark the complete negotiated encoder against the exact legacy codec."""
import hashlib,json,os,platform,random,statistics,subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[3];os.chdir(root)
cpu=min(os.sched_getaffinity(0));os.sched_setaffinity(0,{cpu})
algorithms=['imgui-quic','imgui-quic-adaptive']
jobs=[(trial,scene,algorithm) for trial in range(5) for scene in ['static','moving','dynamic'] for algorithm in algorithms]
random.Random(42).shuffle(jobs);results=[]
for trial,scene,algorithm in jobs:
    row=json.loads(subprocess.check_output(['build/comparison/build/codec',algorithm,scene,'1500']))
    row['trial']=trial;results.append(row)
metadata=dict(system=platform.platform(),compiler=subprocess.check_output(['c++','--version'],text=True).splitlines()[0],
    cpu_affinity=cpu,frames=1500,warmup=30,trials=5,lz4_commit=subprocess.check_output(['git','-C','third_party/lz4','rev-parse','HEAD'],text=True).strip(),
    source_sha256={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in [Path('src/draw_protocol.cpp'),Path('tools/benchmarks/comparison/codec.cpp')]},results=results)
Path('docs/benchmarks/integrated/encode.json').write_text(json.dumps(metadata,indent=2)+'\n')
for scene in ['static','moving','dynamic']:
 for algorithm in algorithms:
    rows=[r for r in results if r['scene']==scene and r['algorithm']==algorithm]
    print(scene,algorithm,*[round(statistics.median(r[k] for r in rows),3) for k in ['mean_bytes','encode_mean_us','encode_p95_us']])
