import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {execFileSync} from 'node:child_process';
import vm from 'node:vm';
const context=vm.createContext({window:{},Uint8Array,DataView,Map});
vm.runInContext(readFileSync('frontend/imgui_quic_draw.js','utf8'),context);
const Decoder=context.window.ImGuiDrawDecoder, decoder=new Decoder();
const fixtures=execFileSync('build/tests/draw_protocol_tests',['--compressed-fixtures'],{maxBuffer:32*1024*1024});
let at=0,count=0,first;
const kinds=new Map();
function record(){const n=fixtures.readUInt32LE(at);at+=4;const b=new Uint8Array(fixtures.subarray(at,at+n));at+=n;return b;}
while(at<fixtures.length){
    const wire=record(),expected=record(),before=[...decoder.frames.entries()],frame=decoder.decode(wire);
    assert.deepEqual(frame.wireBytes,expected);
    assert.deepEqual([...decoder.frames.entries()],before); // decoding alone cannot promote a base
    if(wire[0]===0x23)assert.throws(()=>new Decoder().decode(wire),/missing/);
    if(wire[0]===0x22)first??=wire;
    decoder.commit(frame);assert(decoder.frames.size<=9);
    kinds.set(wire[0],(kinds.get(wire[0])||0)+1);count++;
}
assert.equal(count,240);assert(kinds.get(0x22)>5 && kinds.get(0x23)>100);
const badSize=first.slice();new DataView(badSize.buffer).setUint32(9,16*1024*1024+1,true);
assert.throws(()=>decoder.decode(badSize),/size/);
const badBase=first.slice();new DataView(badBase.buffer).setUint32(5,1,true);
assert.throws(()=>decoder.decode(badBase),/I frame/);
const cached=[...decoder.frames.entries()].map(([id,bytes])=>[id,bytes.slice()]);
for(const n of [13,14,15,first.length-1])assert.throws(()=>decoder.decode(first.subarray(0,n)));
assert.deepEqual([...decoder.frames.entries()],cached);
for(const input of [[],[0xf0],[0x00,0,0],[0x10,1,2,0],[0x1f,1,1,0,255],[0xf0,255,255],[0x10,1]])
    assert.throws(()=>Decoder.lz4(new Uint8Array(input),29));
// Offset-one overlap must repeat the byte. A nonzero view offset is legal.
const repeated=new Uint8Array([0,0,0x1f,42,1,0,4,0x50,42,42,42,42,42]);
assert.deepEqual(Decoder.lz4(repeated.subarray(2),29),new Uint8Array(29).fill(42));
const literals=new Uint8Array([0xf0,14,...new Array(29).fill(7)]);
assert.deepEqual(Decoder.lz4(literals,29),new Uint8Array(29).fill(7));
assert.throws(()=>Decoder.lz4(new Uint8Array([...literals,0]),29));
// Advertise compression only when the server supports it.
const main=readFileSync('frontend/imgui_quic.js','utf8'), sent=[];
const begin=main.indexOf('function sendHelloAck('),end=main.indexOf('\n}',begin)+2;
const negotiation=vm.createContext({drawTransport:true,clientId:1,Uint8Array,DataView,URLSearchParams,
    navigator:{},location:{hash:''},connection:{send:x=>sent.push(x)},window:{}});
vm.runInContext(main.slice(begin,end),negotiation);
for(const caps of [49,113,241]){
    vm.runInContext(`sendHelloAck(${caps})`,negotiation);
    assert.equal(new DataView(sent.pop()).getUint32(5,true)&128,caps&128);
}
// Execute the production renderer and message handler: compressed keyframes
// follow the same presentation-before-ACK path as uncompressed frames.
const acks=[],draws=[];
const gl=new Proxy({}, {get:(_,name)=>(...args)=>{if(name==='drawElements')draws.push(args);}});
const front=vm.createContext({window:{},Uint8Array,DataView,Map,Float32Array,console,
    gl,canvas:{width:1280,height:720},u_proj_loc:0,u_tex_loc:1,vbo:2,ibo:3,a_pos:4,a_uv:5,a_color:6,
    sendDrawAck:id=>acks.push([id,draws.length]),textures:new Map([[1,{}]]),wire:first});
vm.runInContext(readFileSync('frontend/imgui_quic_draw.js','utf8'),front);
vm.runInContext(`var connection={},drawTransport=true,drawDecoder=new window.ImGuiDrawDecoder();
 var lastDpx=0,lastDpy=0,lastDsw=0,lastDsh=0;function ortho(){return new Float32Array(16);}`,front);
vm.runInContext(main.slice(main.indexOf('function readF32'),main.indexOf('const IMGUI_KEY_MAP')),front);
vm.runInContext(main.slice(main.indexOf('    connection.onmessage ='),main.indexOf('\n}\n\n// Some browsers')),front);
vm.runInContext('connection.onmessage({data:wire.buffer})',front);
assert.equal(acks[0][0],new DataView(first.buffer).getUint32(1,true));assert(acks[0][1]>0);
front.wire=badSize;vm.runInContext('connection.onmessage({data:wire.buffer})',front);
assert.equal(acks.at(-1)[0],0);
console.log(`Compressed draw: ${count} exact round trips, aged baselines, packed fallback, bounded LZ4 and presentation ACKs`,Object.fromEntries(kinds));
