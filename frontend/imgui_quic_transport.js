"use strict";
// WebTransport session adapter with message-oriented application handlers.
// Reliable records are length-prefixed; datagrams contain one unframed record.
(() => {
    const MAX_RECORD = 64 * 1024 * 1024 + 1024;
    const u32 = (v, n, at) => v.setUint32(at, n, true);
    class Records {
        constructor() { this.bytes = new Uint8Array(0); }
        feed(chunk, receive) {
            const all = new Uint8Array(this.bytes.length + chunk.length);
            if (all.length > MAX_RECORD + 4) throw Error("transport receive budget");
            all.set(this.bytes); all.set(chunk, this.bytes.length);
            let off = 0;
            while (all.length - off >= 4) {
                const n = new DataView(all.buffer).getUint32(off, true);
                if (!n || n > MAX_RECORD) throw Error("transport record size");
                if (all.length - off < n + 4) break;
                receive(all.slice(off + 4, off + 4 + n));
                off += n + 4;
            }
            this.bytes = all.slice(off);
        }
    }
    class MixedReceiver {
        constructor(deliver, reply) {
            this.deliver = deliver; this.reply = reply;
            this.epoch = 1; this.control = 0; this.frame = 0;
        }
        receive(data, datagram = false) {
            const v = new DataView(data.buffer, data.byteOffset, data.byteLength);
            if (!data.length) throw Error("empty transport record");
            if (data[0] === 1 && !datagram && data.length >= 6) {
                const sequence = v.getUint32(1, true);
                if (sequence !== this.control + 1) throw Error("control ordering");
                this.deliver(data.slice(5));
                this.control = sequence;
                const ack = new Uint8Array(5); ack[0] = 5;
                u32(new DataView(ack.buffer), sequence, 1); this.reply(ack);
            } else if (data[0] === 6 && !datagram && data.length === 5) {
                const epoch = v.getUint32(1, true);
                if (epoch <= this.epoch) throw Error("reset ordering");
                this.epoch = epoch;
                // IDs remain monotonic across recovery; old epochs never present.
            } else if (data[0] === 2 && data.length >= 22) {
                const epoch = v.getUint32(1, true), barrier = v.getUint32(5, true);
                if (epoch !== this.epoch || barrier !== this.control) return;
                const id = v.getUint32(10, true);
                if (id <= this.frame) return;
                if (![13,14,15].includes(data[9]) || (datagram && data[9] === 13))
                    throw Error("draw transport kind");
                this.deliver(data.slice(9));
            } else throw Error("transport message kind");
        }
        ack(id) {
            if (id && id <= this.frame) return null;
            if (id) this.frame = id;
            const msg = new Uint8Array(9); msg[0] = 7;
            const v = new DataView(msg.buffer);
            u32(v, this.epoch, 1); u32(v, id, 5);
            return msg;
        }
    }
    function splitMessages(bytes) {
        if (bytes[0] !== 0x19) return [bytes];
        if (bytes.length < 7) throw Error("input batch");
        const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        const messages = []; let off = 7;
        for (let i = 0; i < v.getUint16(5, true); i++) {
            if (off + 3 > bytes.length) throw Error("input batch header");
            const type = bytes[off], n = v.getUint16(off+1, true); off += 3;
            if (off + n > bytes.length) throw Error("input batch payload");
            const msg = new Uint8Array(5+n); msg[0] = type;
            msg.set(bytes.subarray(1,5),1); msg.set(bytes.subarray(off,off+n),5);
            messages.push(msg); off += n;
        }
        if (off !== bytes.length) throw Error("input batch trailing data");
        return messages;
    }
    class MixedSocket {
        constructor(config) {
            this.url = config.url; this.config = config; this.readyState = 0;
            this.generation = 0; this.pointerSeq = 0; this.pointer = null;
            this.queuedBytes = 0; this.writes = Promise.resolve();
            this.datagramBusy = false; this.pendingPointer = null;
            this.receiver = new MixedReceiver(bytes => this.onmessage?.({data:bytes.buffer}),
                                             bytes => this.write(bytes));
            this.start();
        }
        async start() {
            try {
                const options = {};
                if (this.config.certificateHash) {
                    const hex = this.config.certificateHash;
                    if (!/^[a-fA-F0-9]{64}$/.test(hex)) throw Error("certificate hash");
                    options.serverCertificateHashes = [{algorithm:"sha-256",
                        value:Uint8Array.from(hex.match(/../g), x => parseInt(x,16))}];
                }
                this.transport = new WebTransport(this.config.url, options);
                // Observe closed immediately: rejection may precede ready.
                this.transport.closed.then(() => this.fail("WebTransport disconnected"),
                    () => this.fail("WebTransport connection failed"));
                let timer;
                try {
                    await Promise.race([this.transport.ready, new Promise((_,reject) => {
                        timer = setTimeout(() => reject(Error("WebTransport connection timeout")), 10000);
                    })]);
                } finally { clearTimeout(timer); }
                if (this.readyState === 3) return;
                const stream = await this.transport.createBidirectionalStream();
                this.writer = stream.writable.getWriter();
                this.datagramWriter = this.transport.datagrams.writable.getWriter();
                this.write(new TextEncoder().encode(JSON.stringify({version:1, token:this.config.token,
                    maxDatagramSize:this.transport.datagrams.maxDatagramSize})));
                this.readyState = 1;
                this.onopen?.({});
                this.readStream(stream.readable).catch(() => this.fail());
                this.readDatagrams().catch(() => this.fail());
            } catch (_) { this.fail(); }
        }
        write(bytes) {
            if (this.readyState === 3) return;
            if (!bytes.length || bytes.length > MAX_RECORD || this.queuedBytes + bytes.length > MAX_RECORD) {
                this.fail(); return;
            }
            const record = new Uint8Array(bytes.length+4);
            u32(new DataView(record.buffer), bytes.length, 0); record.set(bytes,4);
            this.queuedBytes += bytes.length;
            this.writes = this.writes.then(async () => {
                if (this.readyState !== 3) await this.writer.write(record);
                this.queuedBytes -= bytes.length;
            }).catch(() => this.fail());
        }
        async readStream(readable) {
            const reader = readable.getReader(), parser = new Records();
            try {
                while (true) {
                    const {value,done} = await reader.read();
                    if (done) break;
                    parser.feed(value, bytes => this.receiver.receive(bytes));
                }
            } finally { reader.releaseLock(); this.fail(); }
        }
        async readDatagrams() {
            const reader = this.transport.datagrams.readable.getReader();
            try {
                while (true) {
                    const {value,done} = await reader.read();
                    if (done) break;
                    this.receiver.receive(value, true);
                }
            } finally { reader.releaseLock(); }
        }
        send(data) {
            if (this.readyState !== 1) return;
            const bytes = data instanceof Uint8Array ? data : new Uint8Array(data);
            for (const msg of splitMessages(bytes)) {
                if (msg[0] === 0x1d) {
                    const ack = this.receiver.ack(new DataView(msg.buffer,msg.byteOffset,msg.byteLength).getUint32(5,true));
                    if (ack) this.write(ack);
                } else if (msg[0] === 0x10) {
                    this.pointer = msg.slice(); this.pointerSeq++;
                    this.motion();
                } else {
                    // Every durable event includes its current pointer position
                    // and a fence against older or overtaking datagrams.
                    const packet = new Uint8Array(18+msg.length), v = new DataView(packet.buffer);
                    packet[0] = 3; u32(v, ++this.generation, 1); u32(v,this.pointerSeq,5);
                    if (this.pointer) { packet.set(this.pointer.subarray(5,13),9); packet[17] = 1; }
                    packet.set(msg,18); this.write(packet);
                }
            }
            if (this.generation >= 0xfffffff0 || this.pointerSeq >= 0xfffffff0) this.fail();
        }
        motion() {
            const packet = new Uint8Array(22), v = new DataView(packet.buffer);
            packet[0] = 4; u32(v,this.generation,1); u32(v,this.pointerSeq,5);
            packet.set(this.pointer,9); this.pendingPointer = packet;
            if (this.datagramBusy) return;
            this.datagramBusy = true;
            (async () => {
                try {
                    while (this.pendingPointer && this.readyState === 1) {
                        const latest = this.pendingPointer; this.pendingPointer = null;
                        await this.datagramWriter.write(latest);
                    }
                } catch (_) { this.fail(); }
                finally { this.datagramBusy = false; }
            })();
        }
        close() { this.fail(); }
        fail(reason = "WebTransport connection failed") {
            if (this.readyState === 3) return;
            this.readyState = 3;
            this.pendingPointer = null;
            this.transport?.close();
            this.onclose?.({reason});
        }
    }
    function create() {
        if (!globalThis.WebTransport)
            throw Error("This browser does not support WebTransport in this context");
        const params = new URLSearchParams(location.hash.slice(1));
        const url = params.get("wt-url"), token = params.get("wt-token");
        if (!url || !token)
            throw Error("Open a configured WebTransport connection link");
        let endpoint;
        try { endpoint = new URL(url); } catch (_) { throw Error("Invalid WebTransport endpoint"); }
        if (endpoint.protocol !== "https:" || endpoint.username || endpoint.password || endpoint.hash)
            throw Error("WebTransport requires an HTTPS endpoint without credentials or a fragment");
        const certificateHash = params.get("wt-cert");
        if (certificateHash && !/^[a-fA-F0-9]{64}$/.test(certificateHash))
            throw Error("Invalid WebTransport certificate hash");
        return new MixedSocket({url:endpoint.href, token, certificateHash});
    }
    window.ImGuiTransport = {create, Records, MixedReceiver, MixedSocket, splitMessages};
})();
