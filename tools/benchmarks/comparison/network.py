#!/usr/bin/env python3
"""Actual imgui-ws and QUICHE stacks, with independent decode/ACK receivers."""
import argparse,asyncio,contextlib,importlib.util,json,os,random,struct,sys,time
from pathlib import Path
import websockets
ROOT=Path(__file__).resolve().parents[3]
spec=importlib.util.spec_from_file_location('transport_bench',ROOT/'tools/benchmarks/transport.py')
b=importlib.util.module_from_spec(spec);spec.loader.exec_module(b)
word=lambda data,at=0:struct.unpack_from('<I',data,at)[0]
pack=struct.pack

def apply_rle(base,delta):
    out=bytearray(base);at=0
    if len(delta)%8:raise ValueError('RLE alignment')
    for off in range(0,len(delta),8):
        count,value=struct.unpack_from('<II',delta,off);size=count*4
        if not count or at+size>len(out):raise ValueError('RLE bounds')
        if value:
            segment=out[at:at+size];mask=pack('<I',value)*count
            out[at:at+size]=(int.from_bytes(segment,'little')^int.from_bytes(mask,'little')).to_bytes(size,'little')
        at+=size
    if at!=len(out):raise ValueError('RLE incomplete output')
    return out

class WS:
    def __init__(self,socket,meter):
        self.ws=socket;self.meter=meter;self.frames=[];self.last_frame=0;self.cache={};self.vars={};self.outer=None;self.errors=[];self.resets=0
    async def pump(self):
        try:
            async for data in self.ws:
                start=time.perf_counter()
                if self.meter.active:self.meter.payload+=len(data)
                if word(data)==1:
                    if self.outer is None:raise ValueError('outer baseline missing')
                    self.outer=pack('<I',0)+apply_rle(self.outer[4:],data[4:])
                else:self.outer=bytearray(data)
                at=4
                while at<len(self.outer):
                    identifier,kind,n=struct.unpack_from('<III',self.outer,at);at+=12
                    if at+n>len(self.outer):raise ValueError('variable bounds')
                    value=self.outer[at:at+n];at+=n
                    self.vars[identifier]=value if kind==0 else apply_rle(self.vars[identifier],value)
                marker=self.vars.get(1)
                if not marker or len(marker)<72 or word(marker,8)<3:continue
                stamp=word(marker,28)|(word(marker,48)<<32)
                if not stamp or stamp==self.meter.last_stamp:continue
                # Translate only the marker into the common metrics interface.
                canonical=bytearray(101)
                struct.pack_into('<I',canonical,57,word(marker,28));struct.pack_into('<I',canonical,77,word(marker,48));struct.pack_into('<I',canonical,97,word(marker,68))
                self.last_frame+=1;self.cache={self.last_frame:canonical};self.frames.append(self.last_frame)
                self.meter.observe(self,b'\0'*9+b'\x0e',time.perf_counter()-start)
        except Exception as error:self.errors.append(error)
    async def requests(self):
        while True:
            await self.ws.send(pack('<I',3));await asyncio.sleep(1/30)
    def mouse(self,seq,y):
        return self.ws.send(pack('<I',4)+f'0 {seq} {y}\0'.encode())

async def run_ws(scene,seconds):
    host=await asyncio.create_subprocess_exec(str(ROOT/'build/comparison/build/ws_host'),scene,stdout=asyncio.subprocess.DEVNULL,stderr=asyncio.subprocess.DEVNULL)
    meter=b.Meter();reader=requests=None
    try:
        for attempt in range(100):
            try:
                socket=await websockets.connect('ws://127.0.0.1:19444/incppect',compression=None,max_size=4*1024*1024,open_timeout=3);break
            except OSError:await asyncio.sleep(.05)
        else:raise RuntimeError('WebSocket host did not listen')
        async with socket:
            client=WS(socket,meter);reader=asyncio.create_task(client.pump())
            await socket.send(pack('<I',1)+b'imgui.n_draw_lists 0 0 imgui.draw_list[%d] 1 1 0 imgui.draw_list[%d] 2 1 1 \0')
            await socket.send(pack('<4I',2,0,1,2));requests=asyncio.create_task(client.requests())
            await b.t.wait(lambda:client.frames,client,seconds=25);await asyncio.sleep(1)
            baseline=b.wire_stats();cpu,_,switches=b.process_usage(host.pid);meter.active=True
            start=time.monotonic();seq=0;next_input=start;next_sample=start
            while time.monotonic()-start<seconds:
                now=time.monotonic()
                if now>=next_input and now-start<seconds-1:
                    seq+=1;meter.inputs[seq]=time.monotonic_ns();await client.mouse(seq,700);next_input+=.1
                if now>=next_sample and meter.last_stamp:
                    meter.display_ages.append((time.monotonic_ns()-meter.last_stamp)/1e6);next_sample+=.02
                if client.errors:raise client.errors[0]
                await asyncio.sleep(.002)
            meter.active=False;elapsed=time.monotonic()-start;end=b.wire_stats();cpu_end,rss,switch_end=b.process_usage(host.pid)
            return dict(frames=meter.frames,fps=round(meter.frames/elapsed,2),wire_KiB_s=round((end['bytes']-baseline['bytes'])/elapsed/1024,2),
                payload_KiB_s=round(meter.payload/elapsed/1024,2),server_cpu_pct=round((cpu_end-cpu)/elapsed*100,2),server_rss_MiB=round(rss,2),
                server_context_switches_s=round((switch_end-switches)/elapsed,2),frame_age_p50_ms=b.percent(meter.frame_ages,.5),frame_age_p95_ms=b.percent(meter.frame_ages,.95),
                display_age_p95_ms=b.percent(meter.display_ages,.95),input_p50_ms=b.percent(meter.input_latencies,.5),input_p95_ms=b.percent(meter.input_latencies,.95),
                input_seen=len(meter.input_latencies),input_sent=seq,client_handle_p50_us=b.percent(meter.decode,.5),drops=end['drops']-baseline['drops'])
    finally:
        for task in [reader,requests]:
            if task:
                task.cancel()
                with contextlib.suppress(asyncio.CancelledError):await task
        if host.returncode is None:host.terminate()
        await asyncio.wait_for(host.wait(),5)

async def main(args):
    if not os.environ.get('IMGW_BENCH_HOST_NETNS') or os.readlink('/proc/self/ns/net')==os.environ['IMGW_BENCH_HOST_NETNS']:
        raise SystemExit('Refusing host namespace; use unshare --user --map-root-user --net')
    b.command('ip','link','set','lo','up');b.command('ip','link','set','lo','mtu','1500');b.command('ethtool','-K','lo','tso','off','gso','off','gro','off')
    profiles=[('local',0,0,0,0),('100ms-1pct',100,5,1,2),('300ms-3pct',300,10,3,2),('100ms-5pct',100,10,5,2)]
    if args.quick:profiles=profiles[:1]
    jobs=[(trial,profile,scene,stack) for trial in range(args.trials) for profile in profiles for scene in args.scenes for stack in ['imgui-quic','imgui-ws']]
    random.Random(42).shuffle(jobs);results=[]
    for trial,(name,delay,jitter,loss,rate),scene,stack in jobs:
        b.network(delay,jitter,loss,rate,42+trial,ports=[('udp',19443),('tcp',19444)])
        row=dict(trial=trial,profile=name,scene=scene,stack=stack,seconds=args.seconds)
        try:row.update(await (b.run_case(scene,args.seconds,'build/transport_benchmark_host') if stack=='imgui-quic' else run_ws(scene,args.seconds)))
        except Exception as error:row['error']=repr(error)
        results.append(row);Path(args.output).parent.mkdir(parents=True,exist_ok=True);Path(args.output).write_text(json.dumps(results,indent=2)+'\n');print(json.dumps(row),flush=True)
    if any('error' in r for r in results):raise SystemExit(1)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--seconds',type=float,default=8);p.add_argument('--trials',type=int,default=2);p.add_argument('--quick',action='store_true');p.add_argument('--scenes',nargs='+',default=['moving','dynamic']);p.add_argument('--output',default='build/comparison/network.json');asyncio.run(main(p.parse_args()))
