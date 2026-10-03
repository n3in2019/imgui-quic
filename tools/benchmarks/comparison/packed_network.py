#!/usr/bin/env python3
"""Compare exact XOR/LZ4 with negotiated integer/planar on actual QUIC."""
import argparse,asyncio,json,os,random
from pathlib import Path
from network import b
async def main(args):
    if not os.environ.get('IMGW_BENCH_HOST_NETNS') or os.readlink('/proc/self/ns/net')==os.environ['IMGW_BENCH_HOST_NETNS']:raise SystemExit('Refusing host namespace')
    b.command('ip','link','set','lo','up');b.command('ip','link','set','lo','mtu','1500');b.command('ethtool','-K','lo','tso','off','gso','off','gro','off')
    profiles=[('local',0,0,0,0),('100ms-5pct',100,10,5,2)]
    jobs=[(trial,p,scene,mode) for trial in range(args.trials) for p in profiles for scene in ['plots','tables'] for mode in ['exact-xor','integer-planar']]
    random.Random(42).shuffle(jobs);rows=[]
    for trial,(name,delay,jitter,loss,rate),scene,mode in jobs:
        b.network(delay,jitter,loss,rate,42+trial)
        os.environ['IMGUI_QUIC_TEST_PRECISION']='integer' if mode=='integer-planar' else 'exact'
        os.environ['IMGUI_QUIC_TEST_PLANAR']='1' if mode=='integer-planar' else '0'
        row=dict(trial=trial,profile=name,scene=scene,mode=mode,seconds=args.seconds)
        try:
            row.update(await b.run_case(scene,args.seconds,'build/transport_benchmark_host'))
            if mode=='integer-planar' and (not row['quantized_frames'] or not row['types'].get(39)):raise RuntimeError('Packed planar mode was not exercised')
        except Exception as error:row['error']=repr(error)
        rows.append(row);out=Path(args.output);out.parent.mkdir(parents=True,exist_ok=True);out.write_text(json.dumps(rows,indent=2)+'\n');print(json.dumps(row),flush=True)
    if any('error' in r for r in rows):raise SystemExit(1)
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--seconds',type=float,default=6);p.add_argument('--trials',type=int,default=2);p.add_argument('--output',default='docs/benchmarks/packed-live/network.json');asyncio.run(main(p.parse_args()))
