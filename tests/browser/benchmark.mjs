import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
import {clockInterval,summarize} from '../../tools/benchmarks/browser/metrics.mjs';
// Native is one million ms ahead: direct subtraction would be catastrophically wrong.
const probe={start_ms:100,end_ms:104,native_ns:'1000102000000'};
const interval=clockInterval([probe],110,0,0);
assert.equal(interval.low_ms,-1000002); assert.equal(interval.high_ms,-999998);
assert.equal(interval.uncertainty_ms,2);
assert.ok(clockInterval([probe],10110,100,1).uncertainty_ms > 3.9);
assert.throws(()=>clockInterval([],0),/no clock/);
assert.throws(()=>clockInterval([probe,{...probe,start_ms:200,end_ms:202}],110,0,0),/inconsistent/);
assert.throws(()=>clockInterval([{...probe,end_ms:99}],110),/invalid/);
const row={id:1,native_ns:'1000100000000',receive_ms:110,decode_start_ms:111,decode_end_ms:112,
    submit_start_ms:113,submit_end_ms:115,raf_callback_ms:120,next_raf_proxy_ms:137,
    superseded_before_proxy:false,gpu_elapsed_ms:null};
const opt={minFrames:1,driftPpm:0,resolutionMs:0,maxUncertaintyMs:3,maxSubmitMs:3,maxProxyMs:40};
const snap={rows:[row],errors:[],visibility:'visible',start_ms:0,stop_ms:1000};
assert.equal(summarize(snap,[probe],opt).status,'PASS');
assert.equal(row.generation_to_receive_ms,10);
assert.equal(row.generation_to_receive_lower_ms,8);assert.equal(row.generation_to_receive_upper_ms,12);
assert.equal(summarize({...snap,rows:[]},[probe],opt).status,'FAIL');
assert.equal(summarize({...snap,rows:[row,{...row}]},[probe],opt).status,'FAIL');
assert.equal(summarize(snap,[probe],{...opt,maxUncertaintyMs:1}).status,'FAIL');
assert.equal(summarize({...snap,rows:[{...row,next_raf_proxy_ms:null}]},[probe],opt).status,'FAIL');
assert.equal(summarize({...snap,rows:[{...row,native_ns:'1000200000000'}]},[probe],opt).status,'FAIL');
// Real observer callbacks with a deterministic browser/GL double, not performance evidence.
let now=100, disjoint=false;
const raf=[],deleted=[];
const ext={TIME_ELAPSED_EXT:1,GPU_DISJOINT_EXT:2,QUERY_RESULT_AVAILABLE_EXT:3,QUERY_RESULT_EXT:4,
    getQueryEXT:()=>64,createQueryEXT:()=>({}),beginQueryEXT(){},endQueryEXT(){},deleteQueryEXT:q=>deleted.push(q),
    getQueryObjectEXT:(_q,type)=>type===3?true:2000000};
const gl={getExtension:()=>ext,getParameter:()=>disjoint,isContextLost:()=>false,getError:()=>0,NO_ERROR:0};
const ctx=vm.createContext({window:{},document:{visibilityState:'visible',addEventListener(){}},
    performance:{now:()=>now++,timeOrigin:0},requestAnimationFrame:f=>raf.push(f),DataView,BigInt});
vm.runInContext(readFileSync(new URL('../../tools/benchmarks/browser/observer.js',import.meta.url),'utf8'),ctx);
const bench=ctx.window.ImGuiBenchmark, bytes=new Uint8Array(101), v=new DataView(bytes.buffer);
v.setUint32(25,1,true);v.setUint32(29,3,true);v.setUint32(57,123,true);v.setUint32(77,456,true);
bench.start();
const a=bench.begin({id:1,bytes},{receivedAt:90,path:'stream'},91,92,gl);bench.end(a,gl);
const b=bench.begin({id:2,bytes},{receivedAt:95,path:'datagram'},96,97,gl);bench.end(b,gl);
while(raf.length) raf.shift()(now);
assert.equal(bench.rows[0].superseded_before_proxy,true);
assert.equal(bench.rows[1].superseded_before_proxy,false);
assert.equal(bench.rows[0].gpu_elapsed_ms,2);
assert.equal(bench.rows[0].native_ns,((456n<<32n)|123n).toString());
disjoint=true;bench.snapshot(gl);
assert.equal(bench.rows[0].gpu_elapsed_ms,null);assert.equal(bench.rows[0].gpu_status,'disjoint');
assert.equal(bench.rows[0].compositor_present_ms,null);assert.equal(deleted.length,2);
bench.stop();assert.equal(bench.begin({id:3,bytes},{},0,0,gl),null);
console.log('benchmark: clock bounds, drift, failure gates, marker, RAF supersession, GPU disjoint PASS');
