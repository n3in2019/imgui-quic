import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
const channels=[],pcs=[],sockets=[];
class PC {
    constructor(config){this.config=config;this.iceGatheringState='complete';this.sctp={maxMessageSize:16384};pcs.push(this);}
    createDataChannel(label,options){const c={label,options,bufferedAmount:0,readyState:'connecting',writes:[],send(b){this.writes.push(new Uint8Array(b));}};channels.push(c);return c;}
    async createOffer(){return {type:'offer',sdp:'test SDP'};}
    async setLocalDescription(d){this.localDescription=d;}
    async setRemoteDescription(d){this.remoteDescription=d;}
    close(){this.closed=true;}
}
class WS {constructor(url){this.url=url;sockets.push(this);}send(d){this.sent=d;}close(){this.closed=true;}}
const ctx=vm.createContext({window:{},RTCPeerConnection:PC,WebSocket:WS,
    Uint8Array,ArrayBuffer,DataView,URLSearchParams,URL,TextEncoder,performance,setTimeout,clearTimeout,
    location:{hash:'#rtc-url=ws%3A%2F%2F192.0.2.1%3A8889%2Fsignal&rtc-token='+'a'.repeat(64)}});
vm.runInContext(readFileSync('frontend/imgui_quic_transport.js','utf8'),ctx);
const api=ctx.window.ImGuiTransport,s=api.create(),[reliable,motion]=channels;
assert.equal(s.label,'WebRTC');assert.deepEqual({...reliable.options},{ordered:true});
assert.deepEqual({...motion.options},{ordered:false,maxRetransmits:0});
assert.deepEqual([...pcs[0].config.iceServers],[]);
let opened=0,closed=0;s.onopen=()=>opened++;s.onclose=()=>closed++;
await sockets[0].onopen();assert.equal(JSON.parse(sockets[0].sent).token,'a'.repeat(64));
await sockets[0].onmessage({data:JSON.stringify({type:'answer',sdp:'answer SDP'})});
assert.equal(pcs[0].remoteDescription.sdp,'answer SDP');
reliable.readyState='open';reliable.onopen();assert.equal(opened,0);
motion.readyState='open';motion.onopen();assert.equal(opened,1);
const bytes=new Uint8Array(50000).fill(42);s.write(bytes);await s.writes;
assert.ok(reliable.writes.every(b=>b.length<=16384));
const joined=Buffer.concat(reliable.writes);assert.equal(joined.readUInt32LE(),50000);assert.deepEqual(joined.subarray(4),Buffer.from(bytes));
// Inherited pointer fences, input batching and draw-after-render ACK format.
const pointer=new Uint8Array(13);pointer[0]=16;new DataView(pointer.buffer).setUint32(1,123,true);s.send(pointer);
assert.equal(motion.writes.length,1);assert.equal(motion.writes[0][0],4);
s.send(new Uint8Array([17,123,0,0,0,0]));await s.writes;
const input=reliable.writes.at(-1);assert.equal(input[4],3);assert.equal(input[21],1);assert.equal(input[22],17);
// Reliable control reassembly accepts split SCTP messages and ACKs only after delivery.
const delivered=[];s.onmessage=e=>delivered.push([...new Uint8Array(e.data)]);
const control=new Uint8Array([6,0,0,0,1,1,0,0,0,6]);
reliable.onmessage({data:control.slice(0,3).buffer});assert.equal(delivered.length,0);
reliable.onmessage({data:control.slice(3).buffer});await s.writes;assert.deepEqual(delivered,[[6]]);
assert.equal(s.receiver.frame,0);
s.close();s.close();assert.equal(closed,1);assert.equal(pcs[0].closed,true);assert.equal(sockets[0].closed,true);
for(const endpoint of ['https://192.0.2.1/signal','ws://192.0.2.1/other','ws://192.0.2.1/signal?token=secret']){
 ctx.location.hash='#rtc-url='+encodeURIComponent(endpoint)+'&rtc-token='+'a'.repeat(64);assert.throws(()=>api.create(),/Invalid/);
}
console.log('WebRTC adapter: HTTP/WS selection, channel policies, chunking, input fences, control reassembly, idempotent close PASS');
