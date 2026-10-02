"""Native Google QUICHE C++ interoperability."""
import asyncio
import os
import re
from pathlib import Path
from types import SimpleNamespace
from aioquic.asyncio import connect
from aioquic.h3.connection import H3_ALPN
from aioquic.quic.configuration import QuicConfiguration
from client import Client, wait, pack, record, MAX_RECORD
ROOT=Path(__file__).resolve().parents[2]

class ManualAuthClient(Client):
    def send(self, data):
        if data.startswith(b'{'):
            self.auth_record = record(data)
        else:
            super().send(data)

class HeldAckClient(Client):
    hold_ack = True
    pending_ack = None
    def send(self, data):
        if data[0] == 7 and self.hold_ack:
            self.pending_ack = data
        else:
            super().send(data)

async def integration():
    cert = ROOT/'build/webtransport_dev/cert.pem'
    key = ROOT/'build/webtransport_dev/key.pem'
    port = 14000 + __import__('os').getpid()%1000
    native = await asyncio.create_subprocess_exec(str(ROOT/'build/examples/example_core_cpp_draw'),
        env={**os.environ,'IMGW_GEOM_DEBUG':'1',
        'IMGUI_QUIC_CERT':str(cert),'IMGUI_QUIC_KEY':str(key),
        'IMGUI_QUIC_TOKEN_FILE':str(ROOT/'build/webtransport_dev/token'),
        'IMGUI_QUIC_ORIGINS':'http://127.0.0.1:8888','IMGUI_QUIC_PORT':str(port+1000)},stdout=asyncio.subprocess.DEVNULL,stderr=asyncio.subprocess.PIPE)
    async with asyncio.timeout(10):
        while True:
            line=await native.stderr.readline()
            if not line:raise RuntimeError('native QUIC startup failed')
            if b'Native WebTransport listening' in line:break
    observed = {}
    async def read_log():
        while line := await native.stderr.readline():
            match = re.search(rb'\[item\] demo-toggle\|([\d.-]+)\|([\d.-]+)\|([\d.-]+)\|([\d.-]+)',line)
            if match:
                rect = [float(v) for v in match.groups()]
                observed['hover'] = ((rect[0]+rect[2])/2,(rect[1]+rect[3])/2)
    log_task = asyncio.create_task(read_log())
    options = SimpleNamespace(token=(ROOT/'build/webtransport_dev/token').read_text().strip(),
                              origin=['http://127.0.0.1:8888'],recovery_timeout=1.1)
    client_config = QuicConfiguration(is_client=True,alpn_protocols=H3_ALPN,max_datagram_frame_size=65536,
        max_data=2*MAX_RECORD,max_stream_data=2*MAX_RECORD)
    client_config.load_verify_locations(cafile=cert)
    try:
        # Origin rejection happens before any native connection is created.
        async with connect('127.0.0.1',port+1000,configuration=client_config,create_protocol=Client) as denied:
            denied.start('https://untrusted.example',options.token)
            await asyncio.wait_for(denied.ready.wait(),3)
            assert denied.status == b'403'
        async with connect('127.0.0.1',port+1000,configuration=client_config,create_protocol=Client) as bad:
            bad.start(options.origin[0],'wrong-token')
            await asyncio.wait_for(bad.ready.wait(),3)
            await asyncio.sleep(.2)
            assert bad.client == 0 and not bad.frames
        async with connect('127.0.0.1',port+1000,configuration=client_config,create_protocol=Client) as client:
            client.start(options.origin[0],options.token)
            await wait(lambda:client.frames,client)
            await wait(lambda:'hover' in observed,client)
            initial = client.frames[-1][2]
            assert client.frames[0][0] == 13
            # Native popup interactions traverse reliable control and real QUIC.
            client.key(561)
            await wait(lambda:client.frames[-1][2]>initial,client)
            client.key(526)
            await wait(lambda:client.frames[-1][2]==initial,client)
            for i in range(40):
                client.mouse(*(observed['hover'] if i%2 else (-100,-100)))
                await asyncio.sleep(.025)
            await wait(lambda:client.datagrams>0,client)
            # Hold several application datagrams, deliver backwards. Older
            # frames must not render or ACK after the newest accepted frame.
            client.hold = True
            for i in range(12):
                client.mouse(*(observed['hover'] if i%2 else (-100,-100)))
                await asyncio.sleep(.025)
            assert client.held
            client.hold = False
            for data in reversed(client.held):
                client.receive(data)
            assert [f[1] for f in client.frames] == sorted(set(f[1] for f in client.frames))
            client.drop = True
            before = client.resets
            for i in range(20):
                client.mouse(*(observed['hover'] if i%2 else (-100,-100)))
                await asyncio.sleep(.025)
            await wait(lambda:client.resets>before,client)
            client.drop = False
            await wait(lambda:client.frames[-1][0]==13,client)
            assert not client.errors,client.errors
            print(f'Native QUIC integration passed: {len(client.frames)} frames, {client.datagrams} datagrams, '
                  'origin/auth rejection, reliable popup input, reordered/dropped P frames, recovery')
        # Bootstrap is single-flight, then the unacknowledged frame window stays bounded.
        async with connect('127.0.0.1',port+1000,configuration=client_config,create_protocol=HeldAckClient) as bounded:
            bounded.start(options.origin[0],options.token)
            await wait(lambda:bounded.frames,bounded)
            for i in range(12):
                bounded.mouse(*(observed['hover'] if i%2 else (-100,-100)))
                await asyncio.sleep(.025)
            assert len(bounded.frames)==1, 'bootstrap must wait for its rendered ACK'
            bounded.hold_ack=False
            bounded.send(bounded.pending_ack)
            bounded.key(561)
            await wait(lambda:len(bounded.frames)>1,bounded)
            bounded.hold_ack=True
            start=len(bounded.frames)
            for i in range(24):
                bounded.mouse(*(observed['hover'] if i%2 else (-100,-100)))
                await asyncio.sleep(.025)
            assert len(bounded.frames)-start<=8, 'unbounded outstanding frame window'
            bounded.hold_ack=False
            if bounded.pending_ack: bounded.send(bounded.pending_ack)
            bounded.key(526)
            await wait(lambda:bounded.frames[-1][2]==bounded.frames[0][2],bounded)
            assert not bounded.errors,bounded.errors
        print('Native bootstrap single-flight and bounded ACK window passed')
        # A malformed authenticated application record closes only its session.
        async with connect('127.0.0.1',port+1000,configuration=client_config,create_protocol=Client) as malformed:
            malformed.start(options.origin[0],options.token)
            await wait(lambda:malformed.frames,malformed)
            malformed.send(b'\xff')
            await asyncio.wait_for(malformed.wait_closed(),3)
        # Closing a bad session releases admission and native state for reconnect.
        async with connect('127.0.0.1',port+1000,configuration=client_config,create_protocol=Client) as again:
            again.start(options.origin[0],options.token)
            await wait(lambda:again.frames,again)
            assert again.frames[0][0]==13 and again.textures
            count=len(again.frames)
            again.send(b'\x07'+pack('<II',again.epoch,0))
            await wait(lambda:again.resets and len(again.frames)>count,again)
            assert again.frames[-1][0]==13
        print('Native session isolation, reconnect and explicit recovery passed')
        # The C++ incremental reader must accept split headers and bodies without
        # treating a partial record as malformed or dispatching it early.
        async with connect('127.0.0.1',port+1000,configuration=client_config,create_protocol=ManualAuthClient) as fragmented:
            fragmented.start(options.origin[0],options.token)
            await asyncio.wait_for(fragmented.ready.wait(),3)
            for chunk in (fragmented.auth_record[:1], fragmented.auth_record[1:3],
                          fragmented.auth_record[3:7], fragmented.auth_record[7:]):
                fragmented._quic.send_stream_data(fragmented.stream,chunk)
                fragmented.transmit()
                await asyncio.sleep(.01)
            await wait(lambda:fragmented.frames,fragmented)
            assert not fragmented.errors
        # Length checks happen immediately on the header, before body allocation.
        async with connect('127.0.0.1',port+1000,configuration=client_config,create_protocol=Client) as oversized:
            oversized.start(options.origin[0],options.token)
            await wait(lambda:oversized.frames,oversized)
            oversized._quic.send_stream_data(oversized.stream,pack('<I',MAX_RECORD+1))
            oversized.transmit()
            await asyncio.wait_for(oversized.wait_closed(),3)
        # An authenticated connection permits exactly one application stream.
        async with connect('127.0.0.1',port+1000,configuration=client_config,create_protocol=Client) as extra:
            extra.start(options.origin[0],options.token)
            await wait(lambda:extra.frames,extra)
            stream=extra.http.create_webtransport_stream(extra.session)
            extra._quic.send_stream_data(stream,b'x')
            extra.transmit()
            await asyncio.wait_for(extra.wait_closed(),3)
        # Opening CONNECT without authenticating cannot occupy admission forever.
        async with connect('127.0.0.1',port+1000,configuration=client_config,create_protocol=ManualAuthClient) as idle:
            idle.start(options.origin[0],options.token)
            await asyncio.wait_for(idle.ready.wait(),3)
            await asyncio.wait_for(idle.wait_closed(),7)
            assert idle.client == 0
        print('QUICHE split records, oversized input, extra stream and authentication deadline passed')
        # More than a read budget of small records, followed by a >256 KiB
        # clipboard and keyboard edges. No extra input should be necessary to
        # resume buffered reads after the last UDP packet has arrived.
        async with connect('127.0.0.1',port+1000,configuration=client_config,create_protocol=Client) as burst:
            burst.start(options.origin[0],options.token)
            await wait(lambda:burst.frames,burst)
            initial=burst.frames[-1][2]
            messages=[b'\x17'+pack('<Ifff',burst.client,1280.,720.,1.)]*96
            text=b'x'*(1024*1024)
            messages.append(b'\x18'+pack('<II',burst.client,len(text))+text)
            messages.extend(bytes([kind])+pack('<IH',burst.client,561) for kind in (0x14,0x15))
            batch=bytearray()
            for msg in messages:
                burst.generation+=1
                batch.extend(record(b'\x03'+pack('<IIffB',burst.generation,0,0.,0.,0)+msg))
            burst._quic.send_stream_data(burst.stream,bytes(batch));burst.transmit()
            await wait(lambda:burst.frames[-1][2]>initial,burst)
            burst.key(526)
            await wait(lambda:burst.frames[-1][2]==initial,burst)
        print('Budget-limited input continuation and large clipboard passed')
    finally:
        native.terminate()
        assert await asyncio.wait_for(native.wait(),5)==0, "native shutdown failed"
        await log_task

if __name__ == '__main__':
    asyncio.run(integration())
