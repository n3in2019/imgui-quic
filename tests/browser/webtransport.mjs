import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
const pending=[];
const ctx=vm.createContext({window:{},Uint8Array,DataView,Map,TextEncoder,URLSearchParams,URL,
    location:{hash:''},setTimeout,clearTimeout,console});
vm.runInContext(readFileSync(new URL('../../frontend/imgui_quic_transport.js',import.meta.url),'utf8'),ctx);
const {Records,MixedReceiver,MixedSocket,splitMessages,create}=ctx.window.ImGuiTransport;
const put=(b,o,n)=>new DataView(b.buffer).setUint32(o,n,true);
const frame=(id,epoch=1,barrier=0)=>{const b=new Uint8Array(22);b[0]=2;put(b,1,epoch);put(b,5,barrier);b[9]=14;put(b,10,id);return b;};
const replies=[],delivered=[];
const r=new MixedReceiver(msg=>{delivered.push(msg);if([13,14,15,0x22,0x23,0x26,0x27].includes(msg[0]))r.ack(new DataView(msg.buffer).getUint32(1,true));},msg=>replies.push(msg));
r.receive(frame(5),true);r.receive(frame(3),true);r.receive(frame(5),true);
assert.equal(delivered.length,1);assert.equal(r.frame,5);
// Resource barriers prevent use of both not-yet-delivered and obsolete textures.
r.receive(frame(6,1,1),true);assert.equal(delivered.length,1);
const control=new Uint8Array([1,1,0,0,0,2]);r.receive(control);
r.receive(frame(6,1,0),true);assert.equal(delivered.length,2);
r.receive(frame(7,1,1),true);assert.equal(delivered.length,3);
assert.equal(replies[0][0],5);
r.receive(new Uint8Array([6,2,0,0,0]));
r.receive(frame(8,1,1),true);assert.equal(delivered.length,3);
r.receive(frame(9,2,1),true);assert.equal(r.frame,9);
assert.equal(new DataView(r.ack(0).buffer).getUint32(1,true),2);
assert.throws(()=>r.receive(control),/ordering/);
assert.throws(()=>r.receive(new Uint8Array([6,1,0,0,0])),/ordering/);
// Compressed I frames remain reliable; compressed P frames retain epoch,
// prerequisite and monotonic presentation checks.
const compressedP=frame(10,2,1);compressedP[9]=0x23;
r.receive(compressedP,true);assert.equal(r.frame,10);
r.receive(compressedP,true);assert.equal(r.frame,10);
const compressedI=frame(11,2,1);compressedI[9]=0x22;
assert.throws(()=>r.receive(compressedI,true),/kind/);
r.receive(compressedI);assert.equal(r.frame,11);
const planarP=frame(12,2,1);planarP[9]=0x27;
r.receive(planarP,true);assert.equal(r.frame,12);
const planarI=frame(13,2,1);planarI[9]=0x26;
assert.throws(()=>r.receive(planarI,true),/kind/);
r.receive(planarI);assert.equal(r.frame,13);
const p=new Records(),outputs=[];
for(const byte of [3,0,0,0,1,2,3,2,0,0,0,4,5])p.feed(new Uint8Array([byte]),x=>outputs.push([...x]));
assert.deepEqual(outputs,[[1,2,3],[4,5]]);
assert.throws(()=>p.feed(new Uint8Array([255,255,255,255]),()=>{}),/size/);
assert.throws(()=>create(), /does not support WebTransport/);
// Use the real send path with a recording transport, no native browser needed.
const s=Object.create(MixedSocket.prototype);
Object.assign(s,{readyState:1,generation:0,pointerSeq:0,pointer:null,receiver:r,
    write:msg=>pending.push(msg),motion(){this.latestMotion=this.pointer.slice();}});
const mouse=new Uint8Array(13);mouse[0]=16;put(mouse,1,42);
new DataView(mouse.buffer).setFloat32(5,100,true);new DataView(mouse.buffer).setFloat32(9,200,true);
s.send(mouse);
const edge=new Uint8Array([17,42,0,0,0,0]);s.send(edge);
assert.equal(pending[0][0],3);assert.equal(pending[0][17],1);
assert.equal(new DataView(pending[0].buffer).getFloat32(9,true),100);
assert.equal(new DataView(pending[0].buffer).getUint32(5,true),1);
assert.deepEqual([...pending[0].slice(18)],[...edge]);
const batch=new Uint8Array(7+3+8+3+1);batch[0]=25;put(batch,1,42);
const b=new DataView(batch.buffer);b.setUint16(5,2,true);batch[7]=16;b.setUint16(8,8,true);batch.set(mouse.slice(5),10);batch[18]=18;b.setUint16(19,1,true);
assert.deepEqual(Array.from(splitMessages(batch),m=>[...m]),[[...mouse],[18,42,0,0,0,0]]);
s.send(batch);assert.equal(s.generation,2);assert.equal(s.pointerSeq,2);
assert.throws(()=>splitMessages(batch.subarray(0,10)),/payload/);
// A rejected QUIC establishment closes once; every retry stays on WebTransport.
ctx.location.hash='#wt-url=https%3A%2F%2Flocalhost%3A4433%2Fwt&wt-token=test';
ctx.WebTransport=class {constructor(){this.ready=Promise.reject(Error('UDP blocked'));this.closed=new Promise(()=>{});}close(){}};
let closed=0;
const failed=create();failed.onclose=()=>closed++;
await new Promise(resolve=>setTimeout(resolve,20));
assert.equal(closed,1);assert.equal(failed.readyState,3);
const retry=create();retry.onclose=()=>closed++;
assert(retry instanceof MixedSocket);
await new Promise(resolve=>setTimeout(resolve,20));
assert.equal(closed,2);assert.equal(retry.readyState,3);
ctx.location.hash='';
assert.throws(()=>create(), /configured WebTransport/);
ctx.location.hash='#wt-url=http%3A%2F%2Flocalhost%2Fwt&wt-token=test';
assert.throws(()=>create(), /HTTPS endpoint/);
ctx.location.hash='#wt-url=https%3A%2F%2Flocalhost%2Fwt&wt-token=test&wt-cert=bad';
assert.throws(()=>create(), /certificate hash/);
console.log('WebTransport frontend passed: framing, stale/duplicate rejection, texture barriers, epochs, pointer fences, input batching, WebTransport-only retries and configuration errors');
// Exercise the production reconnect controller with a native transport adapter.
const source=readFileSync(new URL('../../frontend/imgui_quic.js',import.meta.url),'utf8');
const sockets=[],timers=[];
const ui=vm.createContext({Uint8Array,DataView,console,statusEl:{},
    drawTransport:false,drawDecoder:{reset(){}},clientId:0,pendingSends:[],sendFlushQueued:false,connection:null,
    window:{ImGuiTransport:{create(){const socket={};sockets.push(socket);return socket;}}},
    setTimeout:(fn,ms)=>timers.push({fn,ms}),sendHelloAck(){},resize(){}});
vm.runInContext(source.slice(source.indexOf('let reconnectDelay ='),source.indexOf('// Some browsers require')),ui);
vm.runInContext('connect()',ui);
for(const delay of [1000,2000,4000,8000,15000,15000]){
    sockets.at(-1).onclose();const timer=timers.pop();assert.equal(timer.ms,delay);timer.fn();
}
sockets.at(-1).onopen();assert.match(ui.statusEl.textContent,/negotiating/);
sockets.at(-1).onmessage({data:new Uint8Array([10,73,77,71,87,49,0,0,0]).buffer});
assert.equal(ui.statusEl.textContent,'connected (WebTransport)');
sockets.at(-1).onclose();assert.equal(timers.pop().ms,1000);
ui.window.ImGuiTransport.create=()=>{throw Error('configuration required');};
const count=timers.length;
vm.runInContext('connect()',ui);
assert.equal(ui.statusEl.textContent,'configuration required');assert.equal(timers.length,count);
console.log('WebTransport reconnect controller passed: bounded backoff, negotiation reset, configuration error');

// Optional benchmark metadata is captured at complete-record delivery without
// changing payload bytes, rejection rules, or ACK promotion.
ctx.performance={now:()=>123.5};ctx.window.ImGuiBenchmark={};
let measured;
const meter=new MixedReceiver((bytes,receivedAt,path)=>{measured={bytes,receivedAt,path};},()=>{});
meter.receive(frame(1),true);
assert.equal(measured.receivedAt,123.5);assert.equal(measured.path,'datagram');
assert.deepEqual([...measured.bytes],[...frame(1).slice(9)]);
assert.equal(meter.frame,0); // observation never ACKs on receipt
meter.receive(frame(2));assert.equal(measured.path,'stream');
delete ctx.window.ImGuiBenchmark;
console.log('WebTransport optional benchmark metadata preserves delivery/ACK semantics');
