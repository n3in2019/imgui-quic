import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {execFileSync} from 'node:child_process';
import vm from 'node:vm';
const context=vm.createContext({window:{},Uint8Array,DataView,Map});
vm.runInContext(readFileSync('frontend/imgui_quic_draw.js','utf8'),context);
const Decoder=context.window.ImGuiDrawDecoder,decoder=new Decoder();
const fixtures=execFileSync('build/tests/draw_protocol_tests',['--planar-fixtures'],{maxBuffer:32*1024*1024});
let at=0,count=0,firstI,firstP;
const kinds=new Map(),strides=new Set();
function record(){const n=fixtures.readUInt32LE(at);at+=4;const result=new Uint8Array(fixtures.subarray(at,at+n));at+=n;return result;}
while(at<fixtures.length){
    const wire=record(),expected=record(),before=[...decoder.frames.entries()],frame=decoder.decode(wire);
    assert.deepEqual(frame.wireBytes,expected);
    assert.deepEqual([...decoder.frames.entries()],before);
    if(wire[0]===0x26){firstI??=wire;strides.add(wire[13]);}
    if(wire[0]===0x27){firstP??=wire;strides.add(wire[13]);assert.throws(()=>new Decoder().decode(wire),/missing/);}
    decoder.commit(frame);assert(decoder.frames.size<=9);
    kinds.set(wire[0],(kinds.get(wire[0])||0)+1);count++;
}
assert.equal(count,240);assert(firstI && firstP);assert(strides.has(12) && strides.has(20));
const before=[...decoder.frames.entries()].map(([id,bytes])=>[id,bytes.slice()]);
for(const size of [13,14,15,firstI.length-1])assert.throws(()=>decoder.decode(firstI.subarray(0,size)));
let bad=firstI.slice();bad[13]=0;assert.throws(()=>decoder.decode(bad),/planar/);
bad=firstI.slice();new DataView(bad.buffer).setUint32(5,1,true);assert.throws(()=>decoder.decode(bad),/I frame/);
bad=firstI.slice();new DataView(bad.buffer).setUint32(9,16*1024*1024+1,true);assert.throws(()=>decoder.decode(bad),/size/);
bad=firstI.slice();bad[13]=bad[13]===12?20:12;assert.throws(()=>decoder.decode(bad));
assert.deepEqual([...decoder.frames.entries()],before);
const main=readFileSync('frontend/imgui_quic.js','utf8'),acks=[],draws=[];
const gl=new Proxy({}, {get:(_,name)=>(...args)=>{if(name==='drawElements')draws.push(args);}});
const front=vm.createContext({window:{},Uint8Array,DataView,Map,Float32Array,console,
    gl,canvas:{width:1280,height:720},u_proj_loc:0,u_tex_loc:1,vbo:2,ibo:3,a_pos:4,a_uv:5,a_color:6,
    sendDrawAck:id=>acks.push([id,draws.length]),textures:new Map([[1,{}]]),wire:firstI});
vm.runInContext(readFileSync('frontend/imgui_quic_draw.js','utf8'),front);
vm.runInContext(`var connection={},drawTransport=true,drawDecoder=new window.ImGuiDrawDecoder();
 var lastDpx=0,lastDpy=0,lastDsw=0,lastDsh=0;function ortho(){return new Float32Array(16);}`,front);
vm.runInContext(main.slice(main.indexOf('function readF32'),main.indexOf('const IMGUI_KEY_MAP')),front);
vm.runInContext(main.slice(main.indexOf('    connection.onmessage ='),main.indexOf('\n}\n\n// Some browsers')),front);
vm.runInContext('connection.onmessage({data:wire.buffer})',front);
assert.equal(acks[0][0],new DataView(firstI.buffer).getUint32(1,true));assert(acks[0][1]>0);
front.wire=bad;vm.runInContext('connection.onmessage({data:wire.buffer})',front);
assert.equal(acks.at(-1)[0],0);
console.log('Planar: 240 round trips, 12/20 lanes, aged bases, topology, precision transitions, bounded errors',Object.fromEntries(kinds));
