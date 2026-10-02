"use strict";
// I/P draw transport with optional motion prediction. Cache size exceeds the server's eight-frame window:
// an acknowledged base stays available until every dependent frame arrives.
class ImGuiDrawDecoder {
    constructor() { this.reset(); }
    reset() { this.frames = new Map(); }
    decode(data) {
        if (data.length < 13) throw Error("truncated draw envelope");
        const dv = new DataView(data.buffer, data.byteOffset, data.byteLength);
        const id = dv.getUint32(1, true), baseId = dv.getUint32(5, true);
        const length = dv.getUint32(9, true);
        if (!id || length < 29 || length > 16 * 1024 * 1024) throw Error("invalid draw size/id");
        let bytes;
        if (data[0] === 0x0d) {
            if (baseId || data.length !== 13 + length) throw Error("invalid I frame");
            bytes = data.slice(13);
        } else if (data[0] === 0x0e || data[0] === 0x0f) {
            const base = this.frames.get(baseId);
            if (!base) throw Error("missing P frame base");
            bytes = new Uint8Array(length);
            bytes.set(base.subarray(0, length));
            let off = 13, end = 0;
            if (data[0] === 0x0f) {
                if (data.length < 17) throw Error("truncated motion header");
                const count = dv.getUint32(13, true);
                off = 17;
                if (!count || count > (data.length - off) / 12) throw Error("invalid motion count");
                const ranges = ImGuiDrawDecoder.vertexRanges(base);
                const output = new DataView(bytes.buffer);
                let previous = -1;
                for (let i=0;i<count;i++,off+=12) {
                    const list = dv.getUint32(off,true);
                    const dx = dv.getFloat32(off+4,true), dy = dv.getFloat32(off+8,true);
                    const range = ranges[list];
                    if (list <= previous || !range || !range.count ||
                        range.start + range.count*20 > length || !Number.isFinite(dx) || !Number.isFinite(dy))
                        throw Error("invalid motion range");
                    previous = list;
                    for(let j=0,p=range.start;j<range.count;j++,p+=20) {
                        output.setFloat32(p,output.getFloat32(p,true)+dx,true);
                        output.setFloat32(p+4,output.getFloat32(p+4,true)+dy,true);
                    }
                }
            }
            while (off < data.length) {
                if (off + 8 > data.length) throw Error("truncated patch");
                const start = dv.getUint32(off, true), count = dv.getUint32(off + 4, true);
                off += 8;
                if (!count || start < end || start + count > length || off + count > data.length)
                    throw Error("invalid patch");
                bytes.set(data.subarray(off, off + count), start);
                off += count; end = start + count;
            }
        } else throw Error("unknown draw frame");
        ImGuiDrawDecoder.validate(bytes);
        return { id, bytes };
    }
    commit(frame) {
        this.frames.set(frame.id, frame.bytes);
        while (this.frames.size > 9) this.frames.delete(this.frames.keys().next().value);
    }
    static vertexRanges(bytes) {
        // Baselines have already passed validate() before commit().
        const view = new DataView(bytes.buffer,bytes.byteOffset,bytes.byteLength);
        const ranges = [];
        for(let i=0,off=29;i<view.getUint32(25,true);i++) {
            const nv=view.getUint32(off,true), ni=view.getUint32(off+4,true), nc=view.getUint32(off+8,true);
            ranges.push({start:off+12,count:nv});
            off+=12+nv*20+ni*4+nc*36;
        }
        return ranges;
    }
    static validate(bytes) {
        const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        if (bytes.length < 29 || bytes[0] !== 1) throw Error("invalid draw payload");
        for (let p=1;p<25;p+=4) if (!Number.isFinite(v.getFloat32(p,true))) throw Error("invalid viewport");
        if (v.getFloat32(9,true)<=0 || v.getFloat32(13,true)<=0) throw Error("empty viewport");
        const lists=v.getUint32(25,true);
        let off=29;
        for(let i=0;i<lists;i++) {
            if(off+12>bytes.length) throw Error("truncated list");
            const nv=v.getUint32(off,true), ni=v.getUint32(off+4,true), nc=v.getUint32(off+8,true);
            off+=12;
            const indices=off+nv*20, cmds=indices+ni*4, end=cmds+nc*36;
            if(end>bytes.length) throw Error("truncated geometry");
            for(let c=cmds;c<end;c+=36) {
                for(let p=c;p<c+16;p+=4) if(!Number.isFinite(v.getFloat32(p,true))) throw Error("invalid clip");
                const idx=v.getUint32(c+24,true), vertex=v.getUint32(c+28,true), count=v.getUint32(c+32,true);
                if(idx+count>ni) throw Error("invalid index range");
                for(let j=idx;j<idx+count;j++)
                    if(v.getUint32(indices+j*4,true)+vertex>=nv) throw Error("invalid vertex index");
            }
            off=end;
        }
        if(off!==bytes.length) throw Error("trailing geometry");
    }
}
window.ImGuiDrawDecoder = ImGuiDrawDecoder;
