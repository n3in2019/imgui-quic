#!/usr/bin/env python3
"""Measure whole native server CPU with real QUIC receivers in an isolated namespace."""
import argparse, asyncio, contextlib, hashlib, json, os, platform, random, statistics, subprocess, time
from pathlib import Path
from network import b, ROOT
from aioquic.asyncio import connect
from aioquic.quic.configuration import QuicConfiguration
from aioquic.h3.connection import H3_ALPN

async def measure(host_path, scene, count, seconds, server_cpu):
    credentials=ROOT/'build/webtransport_dev'
    env={k:v for k,v in os.environ.items() if not k.startswith('IMGUI_QUIC_')}
    env.update(IMGUI_QUIC_CERT=str(credentials/'cert.pem'), IMGUI_QUIC_KEY=str(credentials/'key.pem'),
               IMGUI_QUIC_TOKEN_FILE=str(credentials/'token'), IMGW_BENCH_FANOUT='1')
    host=await asyncio.create_subprocess_exec('taskset','-c',str(server_cpu),str(host_path),scene,'30',
        env=env,stdout=asyncio.subprocess.DEVNULL,stderr=asyncio.subprocess.PIPE)
    drain=None
    try:
        async with asyncio.timeout(10):
            while True:
                line=await host.stderr.readline()
                if not line: raise RuntimeError('host startup failed')
                if b'Native WebTransport listening' in line: break
        async def drain_logs():
            while await host.stderr.readline(): pass
        drain=asyncio.create_task(drain_logs())
        async with contextlib.AsyncExitStack() as stack:
            clients=[]
            for _ in range(count):
                config=QuicConfiguration(is_client=True,alpn_protocols=H3_ALPN,max_datagram_frame_size=65536,
                    max_data=2*b.t.MAX_RECORD,max_stream_data=2*b.t.MAX_RECORD)
                config.load_verify_locations(cafile=credentials/'cert.pem')
                meter=b.Meter()
                c=await stack.enter_async_context(connect('127.0.0.1',19443,configuration=config,
                    create_protocol=lambda *a,meter=meter,**kw:b.WT(*a,meter=meter,**kw)))
                c.start('http://localhost',(credentials/'token').read_text().strip())
                await b.t.wait(lambda:c.frames,c,seconds=25)
                clients.append(c)
            await asyncio.sleep(2)
            for c in clients: c.meter.active=True
            start_cpu,rss,_=b.process_usage(host.pid)
            rss_max=rss; start=time.monotonic()
            while time.monotonic()-start<seconds:
                for c in clients:
                    if c.errors: raise c.errors[0]
                _,rss,_=b.process_usage(host.pid);rss_max=max(rss_max,rss)
                await asyncio.sleep(.1)
            elapsed=time.monotonic()-start; end_cpu,rss,_=b.process_usage(host.pid)
            for c in clients:c.meter.active=False
            return {'scene':scene,'clients':count,'elapsed_s':elapsed,
                    'server_cpu_seconds':end_cpu-start_cpu,'server_cpu_pct_one_core':100*(end_cpu-start_cpu)/elapsed,
                    'server_rss_peak_MiB':rss_max,'client_fps':[c.meter.frames/elapsed for c in clients],
                    'client_payload_KiB_s':[c.meter.payload/elapsed/1024 for c in clients],
                    'client_handle_p50_us':[b.percent(c.meter.decode,.5) for c in clients],
                    'client_quantized_frames':[c.quantized_frames for c in clients]}
    finally:
        if host.returncode is None:host.terminate()
        await asyncio.wait_for(host.wait(),5)
        if drain:await drain

async def main(args):
    if args.seconds<=0 or args.trials<1:raise SystemExit('seconds and trials must be positive')
    if not os.environ.get('IMGW_BENCH_HOST_NETNS') or os.readlink('/proc/self/ns/net')==os.environ['IMGW_BENCH_HOST_NETNS']:
        raise SystemExit('Refusing host network namespace')
    b.command('ip','link','set','lo','up')
    cpus=sorted(os.sched_getaffinity(0))
    if len(cpus)<2:raise SystemExit('Need separate server and receiver CPUs')
    server_cpu,receiver_cpu=cpus[:2];os.sched_setaffinity(0,{receiver_cpu})
    os.environ['IMGUI_QUIC_TEST_PRECISION']='integer'
    os.environ['IMGUI_QUIC_TEST_LZ4']='1';os.environ['IMGUI_QUIC_TEST_PLANAR']='1'
    hosts={'stock':Path(args.stock).resolve(),'sse2':Path(args.sse2).resolve()}
    cases=[('dynamic',1),('dynamic',4),('dynamic',8),('plots',1),('dense',1)]
    jobs=[(trial,scene,count,variant) for trial in range(args.trials) for scene,count in cases for variant in hosts]
    random.Random(914).shuffle(jobs)
    report={'requested_seconds':args.seconds,'warmup_seconds':2,'trials':args.trials,'fps_target':30,
        'cpu_basis':'100% equals one logical CPU; /proc/pid/stat user+system time summed across process threads',
        'server_affinity':server_cpu,'receiver_affinity':receiver_cpu,'system':platform.platform(),
        'cpuinfo':Path('/proc/cpuinfo').read_text().split('\n\n')[0],
        'binary_sha256':{name:hashlib.sha256(path.read_bytes()).hexdigest() for name,path in hosts.items()},'results':[]}
    out=Path(args.output);out.parent.mkdir(parents=True,exist_ok=True)
    for trial,scene,count,variant in jobs:
        row=await measure(hosts[variant],scene,count,args.seconds,server_cpu)
        row.update(trial=trial,variant=variant);report['results'].append(row)
        out.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(row),flush=True)
    for scene,count in cases:
        for variant in hosts:
            rows=[r for r in report['results'] if (r['scene'],r['clients'],r['variant'])==(scene,count,variant)]
            print(scene,count,variant,'CPU',round(statistics.median(r['server_cpu_pct_one_core'] for r in rows),2),
                  'FPS',round(statistics.median(statistics.mean(r['client_fps']) for r in rows),2),flush=True)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--stock',default='build/sse2-scalar/transport_benchmark_host')
    p.add_argument('--sse2',default='build/transport_benchmark_host')
    p.add_argument('--seconds',type=float,default=8);p.add_argument('--trials',type=int,default=3)
    p.add_argument('--output',default='docs/benchmarks/server-cpu/results.json')
    asyncio.run(main(p.parse_args()))
