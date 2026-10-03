// Run after codec fixture generation; timing excludes correctness/error comparisons.
import {readFileSync,writeFileSync} from 'node:fs';
import {execFileSync} from 'node:child_process';
import vm from 'node:vm';
import {performance} from 'node:perf_hooks';
import assert from 'node:assert/strict';
const context=vm.createContext({window:{},Uint8Array,DataView,Map});
vm.runInContext(readFileSync('frontend/imgui_quic_draw.js','utf8'),context);
const Decoder=context.window.ImGuiDrawDecoder,results=[];
const scenes=['moving','dynamic','plots','tables','topology','dense'];
const algorithms=['imgui-quic-adaptive','imgui-quic-adaptive-live','imgui-quic-q1-adaptive-live'];
for(const scene of scenes)for(const algorithm of algorithms) {
    const data=execFileSync('build/comparison/build/codec',[algorithm,scene,'120','fixtures'],{maxBuffer:384*1024*1024});
    const records=[];
    for(let at=0;at<data.length;) {
        const n=data.readUInt32LE(at);at+=4;const wire=new Uint8Array(data.subarray(at,at+n));at+=n;
        const m=data.readUInt32LE(at);at+=4;const original=new Uint8Array(data.subarray(at,at+m));at+=m;
        records.push({wire,original});
    }
    for(let trial=0;trial<5;trial++) {
        const decoder=new Decoder(),times=[];
        let maxPositionError=0,maxUVError=0,cacheBytes=0,expandedBytes=0;
        for(let frame=0;frame<records.length;frame++) {
            const {wire,original}=records[frame];
            const begin=performance.now();
            const decoded=decoder.decode(wire);decoder.commit(decoded);
            const elapsed=(performance.now()-begin)*1000;
            if(frame>=30)times.push(elapsed);
            const source=new DataView(original.buffer),dest=new DataView(decoded.bytes.buffer);
            const normalized=decoded.bytes.slice(),normalizedView=new DataView(normalized.buffer);
            for(const r of Decoder.vertexRanges(original))for(let i=0;i<r.count;i++)for(let axis=0;axis<4;axis++) {
                const offset=r.start+i*20+axis*4;
                const error=Math.abs(source.getFloat32(offset,true)-dest.getFloat32(offset,true));
                if(axis<2)maxPositionError=Math.max(maxPositionError,error);else maxUVError=Math.max(maxUVError,error);
                normalizedView.setFloat32(offset,source.getFloat32(offset,true),true);
            }
            assert.deepEqual(normalized,original);
            assert(maxPositionError<=(algorithm.includes('q1')?0.5:0));
            assert(maxUVError<=(algorithm.includes('q1')?1/131070+6e-8:0));
            cacheBytes=[...decoder.frames.values()].reduce((sum,b)=>sum+b.length,0);
            expandedBytes=decoded.bytes.length;
        }
        times.sort((a,b)=>a-b);
        results.push({algorithm,scene,trial,frames:times.length,
            decode_validate_commit_mean_us:times.reduce((a,b)=>a+b,0)/times.length,
            decode_validate_commit_p95_us:times[Math.floor(times.length*.95)],
            cache_bytes:cacheBytes,expanded_frame_bytes:expandedBytes,max_position_error:maxPositionError,max_uv_error:maxUVError});
    }
}
const path=process.argv[2]||'build/comparison/decode.json';
writeFileSync(path,JSON.stringify({runtime:process.version,results},null,2)+'\n');
console.log(`Saved ${results.length} decoder trials to ${path}`);
