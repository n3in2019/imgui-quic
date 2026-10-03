#!/usr/bin/env python3
"""Measure encoder-only costs; does not open transport connections."""
import argparse,hashlib,json,os,platform,random,statistics,subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[3]
p=argparse.ArgumentParser();p.add_argument('--frames',type=int,default=900);p.add_argument('--trials',type=int,default=5)
p.add_argument('--output',default='build/comparison/results.json')
p.add_argument('--algorithms',nargs='+',default=['imgui-quic-adaptive','imgui-quic-adaptive-live','imgui-quic-q1-adaptive-live','imgui-ws','netImgui','RemoteImGui'])
p.add_argument('--scenes',nargs='+',choices=['static','moving','dynamic','plots','tables','topology','dense'],default=['static','moving','dynamic','plots','tables','topology','dense'])
a=p.parse_args()
if a.frames<1 or a.trials<1:p.error('frames and trials must be positive')
pins=json.loads(Path(__file__).with_name('upstream.json').read_text())
for name,pin in pins.items():
    actual=subprocess.check_output(['git','-C',str(root/'build/comparison/upstream'/name),'rev-parse','HEAD'],text=True).strip()
    if actual!=pin['commit']:raise SystemExit(f'Unexpected revision: {name}')
cpu=min(os.sched_getaffinity(0));os.sched_setaffinity(0,{cpu})
jobs=[(trial,scene,algorithm) for trial in range(a.trials) for scene in a.scenes
      for algorithm in a.algorithms]
random.Random(42).shuffle(jobs);results=[]
for trial,scene,algorithm in jobs:
    result=json.loads(subprocess.check_output([str(root/'build/comparison/build/codec'),algorithm,scene,str(a.frames)],text=True))
    result['trial']=trial;results.append(result)
metadata={'upstream':pins,'project_commit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),
          'imgui_commit':subprocess.check_output(['git','-C',str(root/'third_party/imgui'),'rev-parse','HEAD'],text=True).strip(),
          'working_tree_dirty':bool(subprocess.check_output(['git','status','--porcelain'],cwd=root,text=True).strip()),
          'system':platform.platform(),'cpu_affinity':cpu,'cpuinfo':Path('/proc/cpuinfo').read_text().split('\n\n')[0],
          'compiler':subprocess.check_output(['c++','--version'],text=True).splitlines()[0],
          'lz4_commit':subprocess.check_output(['git','-C',str(root/'third_party/lz4'),'rev-parse','HEAD'],text=True).strip(),
          'remote_lz4':'bundled upstream implementation, symbol-prefix adaptation only',
          'source_sha256':{str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in
              [root/'src/draw_protocol.cpp',root/'tools/benchmarks/comparison/codec.cpp',root/'tools/benchmarks/comparison/prepare_remote.py',root/'tools/benchmarks/comparison/scenes.hpp']},
          'scenes':a.scenes,'algorithms':a.algorithms,'frames':a.frames,'trials':a.trials,'warmup_frames':30,'results':results}
out=root/a.output;out.parent.mkdir(parents=True,exist_ok=True);out.write_text(json.dumps(metadata,indent=2)+'\n')
print('scene algorithm bytes/frame encode_mean_us encode_p95_us')
for scene in a.scenes:
 for algorithm in a.algorithms:
    rows=[r for r in results if r['scene']==scene and r['algorithm']==algorithm]
    print(scene,algorithm,*[round(statistics.median(r[k] for r in rows),3) for k in ['mean_bytes','encode_mean_us','encode_p95_us']])
