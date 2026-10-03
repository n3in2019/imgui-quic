#!/usr/bin/env python3
"""Compare isolated encoder experiments; no transport traffic is measured."""
import hashlib,json,os,platform,random,statistics,subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[3]
os.chdir(root)
cpu=min(os.sched_getaffinity(0));os.sched_setaffinity(0,{cpu})
variants={'scalar-reference':('scalar','imgui-quic'),'block-skip':('build','imgui-quic'),'block-skip+lz4':('build','imgui-quic+lz4'),'xor+lz4':('build','imgui-quic-xor-lz4')}
results=[];jobs=[(t,s,v) for t in range(5) for s in ['static','moving','dynamic'] for v in variants]
random.Random(42).shuffle(jobs)
for trial,scene,variant in jobs:
    folder,algorithm=variants[variant]
    r=json.loads(subprocess.check_output([f'build/comparison/{folder}/codec',algorithm,scene,'1500']))
    r.update(trial=trial,variant=variant);results.append(r)
checks={}
for scene in ['static','moving','dynamic']:
    outputs=[subprocess.check_output([f'build/comparison/{folder}/codec','imgui-quic',scene,'300','fixtures']) for folder in ['scalar','build']]
    assert outputs[0]==outputs[1],scene
    checks[scene]=hashlib.sha256(outputs[0]).hexdigest()
    subprocess.run(['build/comparison/build/codec','imgui-quic-xor-lz4',scene,'1500','verify'],check=True,stdout=subprocess.DEVNULL)
Path('build/comparison/review.json').write_text(json.dumps({'system':platform.platform(),'compiler':subprocess.check_output(['c++','--version'],text=True).splitlines()[0],'source_sha256':{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in [Path('src/draw_protocol.cpp'),Path('tools/benchmarks/comparison/codec.cpp')]},'cpu_affinity':cpu,'frames':1500,'trials':5,'fixture_sha256':checks,'results':results},indent=2)+'\n')
for scene in ['static','moving','dynamic']:
 for variant in variants:
    rows=[r for r in results if r['scene']==scene and r['variant']==variant]
    print(scene,variant,*[round(statistics.median(r[k] for r in rows),3) for k in ['mean_bytes','encode_mean_us','encode_p95_us']],flush=True)
