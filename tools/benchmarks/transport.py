#!/usr/bin/env python3
"""Run inside an isolated user/network namespace: see README.md beside this file."""
import argparse
import asyncio
import contextlib
import importlib.util
import json
import os
from pathlib import Path
import statistics
import struct
import subprocess
import time
from aioquic.asyncio import connect
from aioquic.quic.configuration import QuicConfiguration
from aioquic.h3.connection import H3_ALPN

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('quic_test',ROOT/'tests/webtransport/client.py')
t = importlib.util.module_from_spec(spec); spec.loader.exec_module(t)
QUIC = 19443

def command(*args):
    subprocess.run(args,check=True,stdout=subprocess.DEVNULL)

def network(delay, jitter, loss, rate, seed):
    subprocess.run(['tc','qdisc','del','dev','lo','root'],stderr=subprocess.DEVNULL)
    command('tc','qdisc','add','dev','lo','root','handle','1:','prio','bands','3')
    args = ['tc','qdisc','add','dev','lo','parent','1:3','handle','30:','netem','limit','1000']
    if delay:
        args += ['delay',f'{delay/2}ms',f'{jitter}ms']
    if loss:
        args += ['loss','random',f'{loss}%']
    if rate:
        args += ['rate',f'{rate}mbit']
    args += ['seed',str(seed)]
    command(*args)
    for protocol,port in [('udp',QUIC)]:
        for direction in ('src_port','dst_port'):
            command('tc','filter','add','dev','lo','parent','1:','protocol','ip','prio','1',
                    'flower','ip_proto',protocol,direction,str(port),'flowid','1:3')

def wire_stats():
    entries = json.loads(subprocess.check_output(['tc','-s','-j','qdisc','show','dev','lo']))
    q = next(e for e in entries if e['handle']=='30:')
    return {k:q.get(k,0) for k in ('bytes','packets','drops')}

def percent(values,p):
    return round(sorted(values)[min(len(values)-1,int((len(values)-1)*p))],3) if values else None

class Meter:
    def __init__(self):
        self.active = False
        self.frame_ages=[]; self.decode=[]; self.input_latencies=[]; self.display_ages=[]
        self.last_stamp=0; self.last_arrival=0; self.gaps=[]
        self.inputs={}; self.frames=0; self.payload=0; self.types={13:0,14:0,15:0}
    def observe(self,client,packet,elapsed):
        decoded = client.cache[client.last_frame]
        stamp = struct.unpack_from('<I',decoded,57)[0] | (struct.unpack_from('<I',decoded,77)[0]<<32)
        seq = struct.unpack_from('<I',decoded,97)[0]
        now = time.monotonic_ns()
        self.last_stamp = stamp
        if self.active:
            self.frames+=1;self.frame_ages.append((now-stamp)/1e6);self.decode.append(elapsed*1e6)
            if self.last_arrival:
                self.gaps.append((now-self.last_arrival)/1e6)
            self.last_arrival=now
            self.types[packet[9]]+=1
            if seq in self.inputs and self.inputs[seq] is not None:
                self.input_latencies.append((now-self.inputs[seq])/1e6);self.inputs[seq]=None

class WT(t.Client):
    def __init__(self,*a,meter,**kw):
        super().__init__(*a,**kw);self.meter=meter
    def receive(self,data):
        if self.meter.active:self.meter.payload+=len(data)
        before=self.last_frame;start=time.perf_counter()
        super().receive(data)
        if self.last_frame!=before:self.meter.observe(self,data,time.perf_counter()-start)

def process_usage(pid):
    fields=Path(f'/proc/{pid}/stat').read_text().split(') ',1)[1].split()
    cpu=(int(fields[11])+int(fields[12]))/os.sysconf('SC_CLK_TCK')
    rss=int(fields[21])*os.sysconf('SC_PAGE_SIZE')/1024/1024
    switches=0
    for status in Path(f'/proc/{pid}/task').glob('*/status'):
        try:
            switches+=sum(int(line.split()[1]) for line in status.read_text().splitlines()
                          if line.startswith(('voluntary_ctxt_switches:', 'nonvoluntary_ctxt_switches:')))
        except FileNotFoundError: pass
    return cpu,rss,switches

async def measure(client,meter,seconds,pid):
    await t.wait(lambda:client.frames,client,seconds=25)
    await asyncio.sleep(1)
    baseline=wire_stats();cpu_start,_,switch_start=process_usage(pid);meter.active=True
    start=time.monotonic(); seq=0; next_input=start; next_sample=start
    while time.monotonic()-start<seconds:
        now=time.monotonic()
        if now>=next_input and now-start<seconds-1:
            seq+=1;meter.inputs[seq]=time.monotonic_ns();client.mouse(seq,700)
            next_input+=.1
        if now>=next_sample and meter.last_stamp:
            meter.display_ages.append((time.monotonic_ns()-meter.last_stamp)/1e6)
            next_sample+=.02
        if client.errors:raise client.errors[0]
        await asyncio.sleep(.002)
    meter.active=False
    elapsed=time.monotonic()-start;end=wire_stats();cpu_end,rss,switch_end=process_usage(pid)
    return dict(server_context_switches_s=round((switch_end-switch_start)/elapsed,2),server_cpu_pct=round((cpu_end-cpu_start)/elapsed*100,2),server_rss_MiB=round(rss,2),frames=meter.frames,fps=round(meter.frames/elapsed,2),
        frame_age_p50_ms=percent(meter.frame_ages,.5),frame_age_p95_ms=percent(meter.frame_ages,.95),
        display_age_p95_ms=percent(meter.display_ages,.95),max_gap_ms=percent(meter.gaps,1),
        input_p50_ms=percent(meter.input_latencies,.5),input_p95_ms=percent(meter.input_latencies,.95),
        input_seen=len(meter.input_latencies),input_sent=seq,
        payload_KiB_s=round(meter.payload/elapsed/1024,2),
        wire_KiB_s=round((end['bytes']-baseline['bytes'])/elapsed/1024,2),
        packets=end['packets']-baseline['packets'],drops=end['drops']-baseline['drops'],
        client_handle_p50_us=percent(meter.decode,.5),resets=client.resets,types=meter.types)

async def run_case(scene, seconds, host_path):
    env = {k:v for k,v in os.environ.items() if not k.startswith('IMGUI_QUIC_')}
    credentials = ROOT/'build/webtransport_dev'
    env.update(IMGUI_QUIC_CERT=str(credentials/'cert.pem'),
               IMGUI_QUIC_KEY=str(credentials/'key.pem'),
               IMGUI_QUIC_TOKEN_FILE=str(credentials/'token'))
    host = await asyncio.create_subprocess_exec(str(Path(host_path).resolve()), str(scene), '30',
        env=env, stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.PIPE)
    async def drain():
        while await host.stderr.readline(): pass
    log_task = None
    meter = Meter()
    try:
        async with asyncio.timeout(10):
            while True:
                line = await host.stderr.readline()
                if not line: raise RuntimeError('benchmark host failed to start')
                if b'Native WebTransport listening' in line: break
        log_task = asyncio.create_task(drain())
        config = QuicConfiguration(is_client=True, alpn_protocols=H3_ALPN, max_datagram_frame_size=65536,
            max_data=2*t.MAX_RECORD, max_stream_data=2*t.MAX_RECORD)
        config.load_verify_locations(cafile=credentials/'cert.pem')
        async with connect('127.0.0.1', QUIC, configuration=config,
            create_protocol=lambda *a,**kw: WT(*a,meter=meter,**kw)) as client:
            client.start('http://localhost', (credentials/'token').read_text().strip())
            result = await measure(client, meter, seconds, host.pid)
            result['datagrams'] = client.datagrams
            return result
    finally:
        if host.returncode is None: host.terminate()
        await asyncio.wait_for(host.wait(), 5)
        if log_task: await log_task

async def main(args):
    # Never shape the host namespace, even if this script happens to run as root.
    if not os.environ.get('IMGW_BENCH_HOST_NETNS') or os.readlink('/proc/self/ns/net')==os.environ['IMGW_BENCH_HOST_NETNS']:
        raise SystemExit('Refusing host network namespace; use unshare --user --map-root-user --net')
    command('ip','link','set','lo','up')
    command('ip','link','set','lo','mtu','1500')
    command('ethtool','-K','lo','tso','off','gso','off','gro','off')
    profiles=[('local',0,0,0,0),('100ms-1pct',100,5,1,2),('300ms-3pct',300,10,3,2),('100ms-5pct',100,10,5,2)]
    if args.quick:profiles=profiles[:1]
    results=[]
    for trial in range(args.trials):
        for name,delay,jitter,loss,rate in profiles:
            for scene in (0 if s=='moving' else 1 for s in args.scenes):
                network(delay,jitter,loss,rate,42+trial)
                metadata=dict(backend='quiche',seconds=args.seconds,profile=name,
                              scene='moving' if scene==0 else 'dynamic',transport='WT',trial=trial)
                try:result={**metadata,**await run_case(scene,args.seconds,args.host)}
                except Exception as error:result={**metadata,'error':repr(error)}
                results.append(result)
                Path(args.output).parent.mkdir(parents=True,exist_ok=True)
                Path(args.output).write_text(json.dumps(results,indent=2)+'\n')
                print(json.dumps(result),flush=True)
    if any('error' in r for r in results): raise SystemExit(1)

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scenes',nargs='+',choices=['moving','dynamic'],default=['moving','dynamic'])
    parser.add_argument('--host',default='build/transport_benchmark_host')
    parser.add_argument('--seconds',type=float,default=8)
    parser.add_argument('--trials',type=int,default=2)
    parser.add_argument('--quick',action='store_true')
    parser.add_argument('--output',default='build/benchmarks/transport/results.json')
    asyncio.run(main(parser.parse_args()))
