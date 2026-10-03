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
        if (data[0] === 0x26 || data[0] === 0x27) {
            if (data.length < 15 || ![12,20].includes(data[13])) throw Error("invalid planar frame");
            const base = data[0] === 0x27 ? this.frames.get(baseId) : null;
            if (data[0] === 0x26 && baseId) throw Error("invalid planar I frame");
            if (data[0] === 0x27 && !base) throw Error("missing planar P frame base");
            const lanes = ImGuiDrawDecoder.lz4(data.subarray(14), length);
            bytes = new Uint8Array(length);
            let source = 0;
            for (let lane=0;lane<data[13];++lane)
                for(let i=lane;i<length;i+=data[13]) bytes[i]=(lanes[source++]+(base && i<base.length?base[i]:0))&255;
            const stride = bytes[0] === 1 ? 20 : [0x21,0x24,0x25].includes(bytes[0]) ? 12 : 0;
            if (stride !== data[13]) throw Error("planar geometry stride mismatch");
        } else if (data[0] === 0x22 || data[0] === 0x23) {
            const base = data[0] === 0x23 ? this.frames.get(baseId) : null;
            if (data[0] === 0x22 && baseId) throw Error("invalid compressed I frame");
            if (data[0] === 0x23 && !base) throw Error("missing compressed P frame base");
            bytes = ImGuiDrawDecoder.lz4(data.subarray(13), length);
            if (base) {
                const common = Math.min(bytes.length, base.length);
                const output = new DataView(bytes.buffer), input = new DataView(base.buffer, base.byteOffset, base.byteLength);
                let i = 0;
                for (; i + 4 <= common; i += 4) output.setUint32(i, output.getUint32(i, true) ^ input.getUint32(i, true), true);
                for (; i < common; ++i) bytes[i] ^= base[i];
            }
        } else if (data[0] === 0x0d) {
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
                if (base[0] !== 1) throw Error("motion requires float geometry");
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
        const wireBytes = bytes;
        if ([0x21,0x24,0x25].includes(bytes[0])) bytes = ImGuiDrawDecoder.expandQuantized(bytes);
        ImGuiDrawDecoder.validate(bytes);
        return { id, bytes, wireBytes };
    }
    commit(frame) {
        this.frames.set(frame.id, frame.wireBytes || frame.bytes);
        while (this.frames.size > 9) this.frames.delete(this.frames.keys().next().value);
    }
    // Raw LZ4 block, with the decoded length bounded before allocation. Match
    // copies grow geometrically so a long repeated byte is not a scalar loop.
    static lz4(input, length) {
        if (!Number.isInteger(length) || length < 29 || length > 16 * 1024 * 1024 ||
            !input.length || input.length > length + Math.floor(length / 255) + 16)
            throw Error("invalid LZ4 size");
        const output = new Uint8Array(length);
        let ip = 0, op = 0;
        const extended = initial => {
            let size = initial;
            if (initial === 15) {
                let byte;
                do {
                    if (ip >= input.length) throw Error("truncated LZ4 length");
                    byte = input[ip++]; size += byte;
                    if (size > length) throw Error("LZ4 length overflow");
                } while (byte === 255);
            }
            return size;
        };
        while (ip < input.length) {
            const token = input[ip++], literals = extended(token >>> 4);
            if (ip + literals > input.length || op + literals > length) throw Error("invalid LZ4 literals");
            output.set(input.subarray(ip, ip + literals), op); ip += literals; op += literals;
            if (ip === input.length) {
                if (op !== length) throw Error("LZ4 decoded size mismatch");
                return output;
            }
            if (ip + 2 > input.length) throw Error("truncated LZ4 offset");
            const distance = input[ip] | (input[ip + 1] << 8); ip += 2;
            if (!distance || distance > op) throw Error("invalid LZ4 offset");
            const count = extended(token & 15) + 4;
            if (op + count > length) throw Error("LZ4 match overflow");
            const start = op - distance, end = op + count;
            while (op < end) {
                const n = Math.min(op - start, end - op);
                output.copyWithin(op, start, start + n); op += n;
            }
        }
        throw Error("truncated LZ4 block");
    }
    static expandQuantized(bytes) {
        const scale = bytes[0] === 0x21 ? 16 : bytes[0] === 0x24 ? 4 : bytes[0] === 0x25 ? 1 : 0;
        if (!scale) throw Error("invalid quantized encoding");
        const input = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        const lists = input.getUint32(25, true);
        let offset = 29, length = 29;
        const ranges = [];
        for (let i = 0; i < lists; ++i) {
            if (offset + 20 > bytes.length) throw Error("truncated quantized list");
            const nv = input.getUint32(offset, true), ni = input.getUint32(offset + 4, true), nc = input.getUint32(offset + 8, true);
            const ox = input.getFloat32(offset + 12, true), oy = input.getFloat32(offset + 16, true);
            const end = offset + 20 + nv * 12 + ni * 4 + nc * 36;
            length += 12 + nv * 20 + ni * 4 + nc * 36;
            if (end > bytes.length || length > 16 * 1024 * 1024 || !Number.isFinite(ox) || !Number.isFinite(oy))
                throw Error("invalid quantized geometry");
            ranges.push({offset, end, nv, ox, oy});
            offset = end;
        }
        if (offset !== bytes.length) throw Error("trailing quantized geometry");
        const output = new Uint8Array(length), view = new DataView(output.buffer);
        output.set(bytes.subarray(0, 29)); output[0] = 1;
        let out = 29;
        for (const r of ranges) {
            output.set(bytes.subarray(r.offset, r.offset + 12), out); out += 12;
            let p = r.offset + 20;
            for (let i = 0; i < r.nv; ++i, p += 12, out += 20) {
                view.setFloat32(out, r.ox + input.getInt16(p, true) / scale, true);
                view.setFloat32(out + 4, r.oy + input.getInt16(p + 2, true) / scale, true);
                view.setFloat32(out + 8, input.getUint16(p + 4, true) / 65535, true);
                view.setFloat32(out + 12, input.getUint16(p + 6, true) / 65535, true);
                view.setUint32(out + 16, input.getUint32(p + 8, true), true);
                if (!Number.isFinite(view.getFloat32(out, true)) || !Number.isFinite(view.getFloat32(out + 4, true)))
                    throw Error("quantized position overflow");
            }
            output.set(bytes.subarray(p, r.end), out); out += r.end - p;
        }
        return output;
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
