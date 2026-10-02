"""Independent aioquic client for the native WebTransport interoperability tests."""
import asyncio
import json
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
                self.input(b'\x1a'+pack('<II',self.client,49))
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
            if msg[0] == 13:
                assert base == 0 and len(msg) == n+13
                decoded = bytearray(msg[13:])
            else:
                decoded = bytearray(self.cache[base][:n])
                decoded.extend(b'\0'*(n-len(decoded)))
                off = 13
                if msg[0] == 15:
                    ranges = []
                    at = 29
                    for _ in range(word(self.cache[base],25)):
                        nv,ni,nc = struct.unpack_from('<III',self.cache[base],at)
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
            assert decoded[0] == 1 and self.textures
            self.cache[fid] = decoded
            while len(self.cache)>9:
                del self.cache[next(iter(self.cache))]
            self.last_frame = fid
            self.frames.append((msg[0],fid,word(decoded,25)))
            self.send(b'\x07'+pack('<II',self.epoch,fid))

async def wait(predicate, client=None, seconds=6):
    async with asyncio.timeout(seconds):
        while not predicate():
            if client and client.errors:
                raise client.errors[0]
            await asyncio.sleep(.02)
