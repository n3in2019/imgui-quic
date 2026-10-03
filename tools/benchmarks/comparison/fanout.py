#!/usr/bin/env python3
"""Measure native QUIC CPU/RSS with multiple independently ACKing receivers."""
import argparse,asyncio,contextlib,json,os,random,time
from pathlib import Path
from network import b,ROOT
from aioquic.asyncio import connect
from aioquic.quic.configuration import QuicConfiguration
from aioquic.h3.connection import H3_ALPN
async def run(count,scene,seconds):
    credentials=ROOT/'build/webtransport_dev';env={k:v for k,v in os.environ.items() if not k.startswith('IMGUI_QUIC_')}
    env.update(IMGUI_QUIC_CERT=str(credentials/'cert.pem'),IMGUI_QUIC_KEY=str(credentials/'key.pem'),IMGUI_QUIC_TOKEN_FILE=str(credentials/'token'),IMGW_BENCH_FANOUT='1')
    host=await asyncio.create_subprocess_exec(str(ROOT/'build/transport_benchmark_host'),scene,'30',env=env,stdout=asyncio.subprocess.DEVNULL,stderr=asyncio.subprocess.PIPE)
    try:
        async with asyncio.timeout(10):
            while b'Native WebTransport listening' not in await host.stderr.readline():
                if host.returncode is not None:raise RuntimeError('host startup failed')
        async with contextlib.AsyncExitStack() as stack:
            clients=[]
            for i in range(count):
                config=QuicConfiguration(is_client=True,alpn_protocols=H3_ALPN,max_datagram_frame_size=65536,max_data=2*b.t.MAX_RECORD,max_stream_data=2*b.t.MAX_RECORD)
                config.load_verify_locations(cafile=credentials/'cert.pem');meter=b.Meter()
                client=await stack.enter_async_context(connect('127.0.0.1',19443,configuration=config,create_protocol=lambda *a,meter=meter,**kw:b.WT(*a,meter=meter,**kw)))
                client.start('http://localhost',(credentials/'token').read_text().strip());await b.t.wait(lambda:client.frames,client,seconds=25);clients.append(client)
            await asyncio.sleep(1);cpu,_,_=b.process_usage(host.pid);frames=[len(c.frames) for c in clients];start=time.monotonic()
            while time.monotonic()-start<seconds:
                for c in clients:
                    if c.errors:raise c.errors[0]
                await asyncio.sleep(.01)
            elapsed=time.monotonic()-start;cpu_end,rss,_=b.process_usage(host.pid)
            return dict(clients=count,scene=scene,seconds=seconds,server_cpu_pct=round((cpu_end-cpu)/elapsed*100,2),server_rss_MiB=round(rss,2),client_fps=[round((len(c.frames)-n)/elapsed,2) for c,n in zip(clients,frames)])
    finally:
        if host.returncode is None:host.terminate()
        await asyncio.wait_for(host.wait(),5)
async def main(args):
    if args.seconds<=0 or args.trials<1:raise SystemExit('seconds and trials must be positive')
    if not os.environ.get('IMGW_BENCH_HOST_NETNS') or os.readlink('/proc/self/ns/net')==os.environ['IMGW_BENCH_HOST_NETNS']:raise SystemExit('Refusing host namespace')
    Path(args.output).parent.mkdir(parents=True,exist_ok=True)
    b.command('ip','link','set','lo','up');jobs=[(trial,n) for trial in range(args.trials) for n in [1,4,8]];random.Random(42).shuffle(jobs);results=[]
    for trial,n in jobs:
        row=await run(n,'dynamic',args.seconds);row['trial']=trial;results.append(row);Path(args.output).write_text(json.dumps(results,indent=2)+'\n');print(json.dumps(row),flush=True)
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--seconds',type=float,default=6);p.add_argument('--trials',type=int,default=3);p.add_argument('--output',default='build/comparison/fanout.json');asyncio.run(main(p.parse_args()))
