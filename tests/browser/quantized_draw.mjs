import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {execFileSync} from 'node:child_process';
import vm from 'node:vm';
const context=vm.createContext({window:{},Uint8Array,DataView,Map});
vm.runInContext(readFileSync('frontend/imgui_quic_draw.js','utf8'),context);
const Decoder=context.window.ImGuiDrawDecoder;
for(const scale of [16,4,1]) {
const decoder=new Decoder();
const fixture=execFileSync('build/tests/draw_protocol_tests',['--quantized-fixtures',String(scale)],{maxBuffer:16*1024*1024});
let offset=0,count=0,packed=0,fallback=0,maxPosition=0,maxUV=0,firstPacked;
function record(){const size=fixture.readUInt32LE(offset);offset+=4;const value=fixture.subarray(offset,offset+size);offset+=size;return new Uint8Array(value);}
while(offset<fixture.length){
    const wire=record(),original=record(),frame=decoder.decode(wire);
    if([0x21,0x24,0x25].includes(frame.wireBytes[0])){packed++;firstPacked??=frame.wireBytes.slice();}else fallback++;
    const source=new DataView(original.buffer),output=new DataView(frame.bytes.buffer);
    assert.equal(original.length,frame.bytes.length);
    const normalized=frame.bytes.slice(),view=new DataView(normalized.buffer);
    for(const range of Decoder.vertexRanges(original))for(let i=0;i<range.count;i++){
        const p=range.start+i*20;
        for(let j=0;j<4;j++){
            const error=Math.abs(source.getFloat32(p+j*4,true)-output.getFloat32(p+j*4,true));
            if(j<2){maxPosition=Math.max(maxPosition,error);assert(error<=0.5/scale);}
            else {maxUV=Math.max(maxUV,error);assert(error<=1/131070+6e-8);}
            view.setFloat32(p+j*4,source.getFloat32(p+j*4,true),true);
        }
    }
    assert.deepEqual(normalized,original); // colors, indices, clips, textures and offsets stay exact
    decoder.commit(frame); count++;
}
assert.equal(count,150);assert(packed>100 && fallback>10);
assert.throws(()=>Decoder.expandQuantized(firstPacked.subarray(0,firstPacked.length-1)));
let bad=firstPacked.slice();new DataView(bad.buffer).setUint32(29,0xffffffff,true);
assert.throws(()=>Decoder.expandQuantized(bad));
bad=firstPacked.slice();new DataView(bad.buffer).setFloat32(41,Infinity,true);
assert.throws(()=>Decoder.expandQuantized(bad));
// Neither rejected frames nor failed presentation may replace an acknowledged base.
const before=[...decoder.frames.entries()];
assert.throws(()=>decoder.decode(new Uint8Array(13)));
assert.deepEqual([...decoder.frames.entries()],before);
console.log(`Quantized ${scale}: ${count} frames (${packed} packed, ${fallback} fallback), max position error ${maxPosition}, max UV error ${maxUV}`);
}
// Default and explicitly selected precision negotiate only advertised modes.
const main=readFileSync('frontend/imgui_quic.js','utf8');
const begin=main.indexOf('function sendHelloAck('),end=main.indexOf('\n}',begin)+2;
const sent=[];
const negotiation=vm.createContext({drawTransport:true,clientId:1,Uint8Array,DataView,URLSearchParams,
    navigator:{},location:{hash:'#draw-quantized=1'},connection:{send:x=>sent.push(x)},window:{}});
vm.runInContext(main.slice(begin,end),negotiation);
vm.runInContext('sendHelloAck(113)',negotiation);
assert(new DataView(sent.pop()).getUint32(5,true)&64);
vm.runInContext('sendHelloAck(49)',negotiation);
assert.equal(new DataView(sent.pop()).getUint32(5,true)&64,0);
negotiation.location.hash='';vm.runInContext('sendHelloAck(113)',negotiation);
assert.equal(new DataView(sent.pop()).getUint32(5,true)&64,64);
for(const [hash,bit] of [['',512],['#draw-precision=integer',512],['#draw-precision=quarter',256],['#draw-precision=fine',64],['#draw-precision=exact',0],['#draw-quantized=0',0]]) {
    negotiation.location.hash=hash;vm.runInContext('sendHelloAck(2033)',negotiation);
    assert.equal(new DataView(sent.pop()).getUint32(5,true)&(64|256|512),bit);
}
negotiation.location.hash='';vm.runInContext('sendHelloAck(433)',negotiation);
assert.equal(new DataView(sent.pop()).getUint32(5,true)&(64|256|512),256);
negotiation.location.hash='#draw-precision=fine';vm.runInContext('sendHelloAck(433)',negotiation);
assert.equal(new DataView(sent.pop()).getUint32(5,true)&(64|256|512),0);
