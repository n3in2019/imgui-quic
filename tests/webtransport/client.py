"""Independent aioquic client for the native WebTransport interoperability tests."""
import asyncio
import json
import os
import struct
from aioquic.asyncio import QuicConnectionProtocol
from aioquic.h3.connection import H3Connection
from aioquic.h3.events import HeadersReceived, WebTransportStreamDataReceived, DatagramReceived
from aioquic.quic.events import ProtocolNegotiated, StreamDataReceived
pack = struct.pack
word = lambda data, offset=0: struct.unpack_from('<I', data, offset)[0]
MAX_RECORD = 64 * 1024 * 1024 + 1024

def record(data):
    return pack('<I',len(data))+data

def expand_quantized(data):
    scale={0x21:16,0x24:4,0x25:1}[data[0]]
    result=bytearray(data[:29]);result[0]=1;offset=29
    for _ in range(word(data,25)):
        nv,ni,nc,ox,oy=struct.unpack_from('<IIIff',data,offset)
        result.extend(data[offset:offset+12]);offset+=20
        for _ in range(nv):
            x,y,u,v,color=struct.unpack_from('<hhHHI',data,offset);offset+=12
            result.extend(pack('<ffffI',ox+x/scale,oy+y/scale,u/65535,v/65535,color))
        size=ni*4+nc*36;result.extend(data[offset:offset+size]);offset+=size
    assert offset==len(data)
    return result

def decode_lz4(data,n):
    assert 29 <= n <= 16*1024*1024
    result=bytearray();at=0
    def length(size):
        nonlocal at
        if size==15:
            while True:
                assert at<len(data)
                byte=data[at];at+=1;size+=byte
                assert size<=n
                if byte!=255:break
        return size
    while at<len(data):
        token=data[at];at+=1
        size=length(token>>4)
        assert at+size<=len(data) and len(result)+size<=n
        result.extend(data[at:at+size]);at+=size
        if at==len(data):
            assert len(result)==n
            return result
        assert at+2<=len(data)
        distance=data[at]|data[at+1]<<8;at+=2
        assert 0<distance<=len(result)
        size=length(token&15)+4
        assert len(result)+size<=n
        result.extend((result[-distance:] * ((size+distance-1)//distance))[:size])
    raise ValueError('truncated LZ4')

class Records:
    def __init__(self): self.buffer=bytearray()
    def feed(self,data):
        self.buffer.extend(data)
        if len(self.buffer)>MAX_RECORD+4: raise ValueError('record budget')
        while len(self.buffer)>=4:
            n=word(self.buffer)
            if not 0<n<=MAX_RECORD: raise ValueError('record size')
            if len(self.buffer)<n+4: break
            data=bytes(self.buffer[4:4+n]);del self.buffer[:4+n]
            yield data

def move(client,x=80.,y=90.):
    return b'\x10'+pack('<Iff',client,x,y)

class Client(QuicConnectionProtocol):
    def __init__(self,*args,**kwargs):
        super().__init__(*args,**kwargs)
        self.http = None
        self.parser = Records()
        self.ready = asyncio.Event()
        self.client = 0
        self.generation = 0
        self.pointer_sequence = 0
        self.pointer = None
        self.epoch = 1
        self.control = 0
        self.last_frame = 0
        self.cache = {}
        self.wire_cache = {}
        self.quantized_frames = 0
        self.frames = []
        self.errors = []
        self.datagrams = 0
        self.drop = False
        self.held = []
        self.hold = False
        self.resets = 0
        self.textures = set()

    def start(self, origin, token):
        self.token = token
        self.session = self._quic.get_next_available_stream_id()
        self.http.send_headers(self.session, [(b':method',b'CONNECT'),(b':scheme',b'https'),
            (b':authority',b'localhost'),(b':path',b'/wt'),(b':protocol',b'webtransport'),
            (b'origin',origin.encode())])
        self.transmit()

    def send(self, data):
        self._quic.send_stream_data(self.stream,record(data))
        self.transmit()

    def input(self, msg):
        self.generation += 1
        x,y = self.pointer or (0,0)
        self.send(b'\x03'+pack('<IIffB',self.generation,self.pointer_sequence,x,y,self.pointer is not None)+msg)

    def mouse(self,x,y):
        self.pointer = (x,y)
        self.pointer_sequence += 1
        self.http.send_datagram(self.session,b'\x04'+pack('<II',self.generation,self.pointer_sequence)+move(self.client,x,y))
        self.transmit()

    def key(self,key):
        for kind in (0x14,0x15):
            self.input(bytes([kind])+pack('<IH',self.client,key))

    def quic_event_received(self,event):
        # aioquic does not register the receive half of a locally created WT
        # bidirectional stream. Its return bytes are raw application records.
        if isinstance(event,StreamDataReceived) and event.stream_id == getattr(self,'stream',None):
            try:
                for data in self.parser.feed(event.data):
                    self.receive(data)
            except Exception as error:
                self.errors.append(error)
            return
        if isinstance(event,ProtocolNegotiated):
            self.http = H3Connection(self._quic,enable_webtransport=True)
        if self.http is None:
            return
        try:
            for item in self.http.handle_event(event):
                if isinstance(item,HeadersReceived):
                    self.status = dict(item.headers)[b':status']
                    if self.status == b'200':
                        self.stream = self.http.create_webtransport_stream(self.session)
                        self.send(json.dumps({'version':1,'token':self.token,'maxDatagramSize':1100}).encode())
                    self.ready.set()
                elif isinstance(item,WebTransportStreamDataReceived):
                    for data in self.parser.feed(item.data):
                        self.receive(data)
                elif isinstance(item,DatagramReceived):
                    self.datagrams += 1
                    if self.drop:
                        continue
                    if self.hold:
                        self.held.append(item.data)
                    else:
                        self.receive(item.data)
        except Exception as error:
            self.errors.append(error)

    def receive(self,data):
        if data[0] == 1:
            sequence = word(data,1)
            assert sequence == self.control+1
            msg = data[5:]
            if msg[0] == 6:
                self.client = word(msg,1)
            elif msg[0] == 10:
                mode=os.environ.get('IMGUI_QUIC_TEST_PRECISION','fine' if os.environ.get('IMGUI_QUIC_TEST_QUANTIZED')=='1' else 'exact')
                quantized={'fine':64,'quarter':256,'integer':512,'exact':0}[mode]&word(msg,5)
                if os.environ.get('IMGUI_QUIC_TEST_PLANAR','1')=='1':quantized|=word(msg,5)&1024
                self.input(b'\x1a'+pack('<II',self.client,49 | (128 if os.environ.get('IMGUI_QUIC_TEST_LZ4','1')=='1' and word(msg,5)&128 else 0) | quantized))
            elif msg[0] == 2:
                self.textures.add(struct.unpack_from('<Q',msg,1)[0])
            self.control = sequence
            self.send(b'\x05'+pack('<I',sequence))
        elif data[0] == 6:
            self.epoch = word(data,1)
            self.resets += 1
        elif data[0] == 2:
            if word(data,1) != self.epoch or word(data,5) != self.control:
                return
            msg = data[9:]
            fid,base,n = struct.unpack_from('<III',msg,1)
            if fid <= self.last_frame:
                return
            assert 29 <= n <= 16*1024*1024
            if msg[0] in (0x26,0x27):
                assert len(msg)>14 and msg[13] in (12,20)
                assert (base==0) if msg[0]==0x26 else (base in self.wire_cache)
                lanes=decode_lz4(msg[14:],n);decoded=bytearray(n);source=0
                old=self.wire_cache[base] if base else b''
                for lane in range(msg[13]):
                    for i in range(lane,n,msg[13]):
                        decoded[i]=(lanes[source]+(old[i] if i<len(old) else 0))&255;source+=1
                assert msg[13]==(20 if decoded[0]==1 else 12)
            elif msg[0] in (0x22,0x23):
                assert (base==0) if msg[0]==0x22 else (base in self.wire_cache)
                decoded=decode_lz4(msg[13:],n)
                if msg[0]==0x23:
                    old=self.wire_cache[base]
                    common=min(n,len(old))
                    decoded[:common]=(int.from_bytes(decoded[:common],'little') ^ int.from_bytes(old[:common],'little')).to_bytes(common,'little')
            elif msg[0] == 13:
                assert base == 0 and len(msg) == n+13
                decoded = bytearray(msg[13:])
            else:
                decoded = bytearray(self.wire_cache[base][:n])
                decoded.extend(b'\0'*(n-len(decoded)))
                off = 13
                if msg[0] == 15:
                    ranges = []
                    at = 29
                    for _ in range(word(self.wire_cache[base],25)):
                        nv,ni,nc = struct.unpack_from('<III',self.wire_cache[base],at)
                        ranges.append((at+12,nv)); at += 12+nv*20+ni*4+nc*36
                    count = word(msg,13); off = 17
                    for _ in range(count):
                        index,dx,dy = struct.unpack_from('<Iff',msg,off); off += 12
                        at,nv = ranges[index]
                        for j in range(nv):
                            x,y = struct.unpack_from('<ff',decoded,at+j*20)
                            struct.pack_into('<ff',decoded,at+j*20,x+dx,y+dy)
                while off < len(msg):
                    at,count = struct.unpack_from('<II',msg,off);off += 8
                    assert at+count <= n and off+count <= len(msg)
                    decoded[at:at+count] = msg[off:off+count];off += count
            self.wire_cache[fid] = decoded
            if decoded[0] in (0x21,0x24,0x25):
                decoded = expand_quantized(decoded)
                self.quantized_frames += 1
            assert decoded[0] == 1 and self.textures
            self.cache[fid] = decoded
            while len(self.cache)>9:
                oldest=next(iter(self.cache));del self.cache[oldest];del self.wire_cache[oldest]
            self.last_frame = fid
            self.frames.append((msg[0],fid,word(decoded,25)))
            self.send(b'\x07'+pack('<II',self.epoch,fid))

async def wait(predicate, client=None, seconds=6):
    async with asyncio.timeout(seconds):
        while not predicate():
            if client and client.errors:
                raise client.errors[0]
            await asyncio.sleep(.02)
