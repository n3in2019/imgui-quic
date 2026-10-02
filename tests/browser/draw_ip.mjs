import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {execFileSync} from 'node:child_process';
import vm from 'node:vm';
import path from 'node:path';
const root=path.resolve(import.meta.dirname,'../..');
const context=vm.createContext({window:{},Uint8Array,DataView,Map});
vm.runInContext(readFileSync(path.join(root,'frontend/imgui_quic_draw.js'),'utf8'),context);
const Decoder=context.window.ImGuiDrawDecoder;
const decoder=new Decoder();
const fixtures=execFileSync(path.join(root,'build/tests/draw_protocol_tests'),['--fixtures'],{maxBuffer:16*1024*1024});
let off=0,frames=0,pframes=0,motionFrames=0;
function record(){const n=fixtures.readUInt32LE(off);off+=4;const bytes=new Uint8Array(fixtures.subarray(off,off+n));off+=n;return bytes;}
while(off<fixtures.length){
    const wire=record(), expected=record(), decoded=decoder.decode(wire);
    assert.deepEqual(decoded.bytes,expected); decoder.commit(decoded);
    frames++;if(wire[0]===0x0e || wire[0]===0x0f)pframes++;if(wire[0]===0x0f)motionFrames++;
    assert(decoder.frames.size<=9);
    assert.throws(()=>new Decoder().decode(wire.subarray(0,12)));
    if(wire[0]===0x0e || wire[0]===0x0f) assert.throws(()=>new Decoder().decode(wire),/missing/);
}
assert(pframes>100);assert(motionFrames>50);
// A malformed patch must not mutate the cached baseline.
const base=[...decoder.frames.entries()].at(-1), original=base[1].slice();
const bad=new Uint8Array(22),v=new DataView(bad.buffer);
bad[0]=0x0e;v.setUint32(1,999,true);v.setUint32(5,base[0],true);v.setUint32(9,base[1].length,true);
v.setUint32(13,base[1].length,true);v.setUint32(17,1,true);
assert.throws(()=>decoder.decode(bad));assert.deepEqual(decoder.frames.get(base[0]),original);

// Exercise the production message handler and renderer with a recording GL
// stub. This verifies dispatch, presentation-before-ACK, and recovery without
// requiring a browser.
const main=readFileSync(path.join(root,'frontend/imgui_quic.js'),'utf8');
const acks=[],draws=[],attribs=[];
const gl=new Proxy({}, {get:(_,name)=> (...args)=>{
    if(name==='drawElements')draws.push(args);
    if(name==='vertexAttribPointer')attribs.push(args);
}});
const front=vm.createContext({window:{},Uint8Array,DataView,Map,Float32Array,console,
    gl,canvas:{width:1280,height:720},u_proj_loc:0,u_tex_loc:1,vbo:2,ibo:3,
    a_pos:4,a_uv:5,a_color:6,sendDrawAck:id=>acks.push(id),
    textures:new Map([[1,{}]]),wire:new Uint8Array(fixtures.subarray(4,4+fixtures.readUInt32LE(0)))});
vm.runInContext(readFileSync(path.join(root,'frontend/imgui_quic_draw.js'),'utf8'),front);
vm.runInContext(`var connection={},drawTransport=true,drawDecoder=new window.ImGuiDrawDecoder();
    var lastDpx=0,lastDpy=0,lastDsw=0,lastDsh=0;
    function ortho(){return new Float32Array(16);}`,front);
vm.runInContext(main.slice(main.indexOf('function readF32'),main.indexOf('const IMGUI_KEY_MAP')),front);
vm.runInContext(main.slice(main.indexOf('    connection.onmessage ='),main.indexOf('\n}\n\n// Some browsers')),front);
vm.runInContext('connection.onmessage({data:wire.buffer})',front);
assert.deepEqual(acks,[1]);assert(draws.length>0);
assert.equal(vm.runInContext('lastDsw',front),1280);
// WebGL1 implements base vertex through attribute byte offsets.
vm.runInContext(`mediaLastFrame.lists[0].cmds[0].vtxOff=3;
    renderFromParsed(mediaLastFrame);`,front);
assert(attribs.some(a=>a[0]===4 && a[5]===60));
front.wire=new Uint8Array([14]);
vm.runInContext('connection.onmessage({data:wire.buffer})',front);
assert.equal(acks.at(-1),0);

// Motion records must be bounded and cannot address non-vertex bytes.
const motionBase=[...decoder.frames.entries()].at(-1);
const invalidMotion=new Uint8Array(29),mv=new DataView(invalidMotion.buffer);
invalidMotion[0]=15;mv.setUint32(1,1000,true);mv.setUint32(5,motionBase[0],true);
mv.setUint32(9,motionBase[1].length,true);mv.setUint32(13,1,true);
mv.setUint32(17,0xffffffff,true);
assert.throws(()=>decoder.decode(invalidMotion),/motion/);
mv.setUint32(17,0,true);mv.setFloat32(21,NaN,true);
assert.throws(()=>decoder.decode(invalidMotion),/motion/);
assert.throws(()=>decoder.decode(invalidMotion.subarray(0,25)),/motion/);

console.log(`I/P tests passed: ${frames} cross-language frames (${pframes} P, ${motionFrames} motion), renderer ACK ordering and malformed-frame recovery`);
