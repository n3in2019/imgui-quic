"use strict";

let drawTransport = false;
const drawDecoder = window.ImGuiDrawDecoder ? new window.ImGuiDrawDecoder() : null;
function sendDrawAck(id) {
    const bytes = new ArrayBuffer(9), view = new DataView(bytes);
    view.setUint8(0, 0x1d); view.setUint32(1, clientId, true); view.setUint32(5, id, true);
    connection.send(bytes);
}

const canvas = document.getElementById("canvas");
const statusEl = document.getElementById("status");
// preserveDrawingBuffer lets the canvas be screenshotted (otherwise the GL
// backbuffer is cleared after compositing and captures come back blank).
const gl = canvas.getContext("webgl", { alpha: false, antialias: false, premultipliedAlpha: false, preserveDrawingBuffer: true });

if (!gl) {
    document.body.innerText = "WebGL not supported";
    throw new Error("No WebGL");
}

const extUintIdx = gl.getExtension("OES_element_index_uint");
if (!extUintIdx) {
    statusEl.textContent = "WebGL extension OES_element_index_uint is required";
    statusEl.className = "disconnected";
    throw new Error("OES_element_index_uint not available");
}

const VERT_SRC = `
attribute vec2 a_pos;
attribute vec2 a_uv;
attribute vec4 a_color;
uniform mat4 u_proj;
varying vec2 v_uv;
varying vec4 v_color;
void main() {
    gl_Position = u_proj * vec4(a_pos, 0.0, 1.0);
    v_uv = a_uv;
    v_color = a_color;
}`;

const FRAG_SRC = `
precision mediump float;
uniform sampler2D u_tex;
varying vec2 v_uv;
varying vec4 v_color;
void main() {
    gl_FragColor = v_color * texture2D(u_tex, v_uv);
}`;

function compileShader(src, type) {
    const s = gl.createShader(type);
    gl.shaderSource(s, src);
    gl.compileShader(s);
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) {
        console.error("Shader compile error:", gl.getShaderInfoLog(s));
        return null;
    }
    return s;
}

const prog = gl.createProgram();
gl.attachShader(prog, compileShader(VERT_SRC, gl.VERTEX_SHADER));
gl.attachShader(prog, compileShader(FRAG_SRC, gl.FRAGMENT_SHADER));
gl.linkProgram(prog);
if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) {
    console.error("Shader link error:", gl.getProgramInfoLog(prog));
}
gl.useProgram(prog);

const a_pos = gl.getAttribLocation(prog, "a_pos");
const a_uv = gl.getAttribLocation(prog, "a_uv");
const a_color = gl.getAttribLocation(prog, "a_color");
const u_proj_loc = gl.getUniformLocation(prog, "u_proj");
const u_tex_loc = gl.getUniformLocation(prog, "u_tex");

const vbo = gl.createBuffer();
const ibo = gl.createBuffer();

gl.enable(gl.BLEND);
gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
gl.enable(gl.SCISSOR_TEST);
gl.disable(gl.DEPTH_TEST);

const textures = new Map();

let lastDpx = 0, lastDpy = 0, lastDsw = 0, lastDsh = 0;

function uploadTexture(id, width, height, pixels) {
    let tex = textures.get(id);
    if (!tex) {
        tex = gl.createTexture();
        textures.set(id, tex);
    }
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, width, height, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array(pixels));
}

function ortho(l, r, b, t) {
    const out = new Float32Array(16);
    out[0] = 2.0 / (r - l);
    out[5] = 2.0 / (t - b);
    out[10] = -1.0;
    out[12] = -(r + l) / (r - l);
    out[13] = -(t + b) / (t - b);
    out[15] = 1.0;
    return out;
}

function readF32(dv, off) { return [dv.getFloat32(off, true), off + 4]; }
function readU32(dv, off) { return [dv.getUint32(off, true), off + 4]; }
function readU64(dv, off) {
    const lo = dv.getUint32(off, true);
    const hi = dv.getUint32(off + 4, true);
    return [lo + hi * 0x100000000, off + 8];
}

function parseDrawLists(data) {
    const dv = new DataView(data.buffer, data.byteOffset, data.byteLength);
    let off = 1;

    const [dpx] = readF32(dv, off); off += 4;
    const [dpy] = readF32(dv, off); off += 4;
    const [dsw] = readF32(dv, off); off += 4;
    const [dsh] = readF32(dv, off); off += 4;
    const [fbsx] = readF32(dv, off); off += 4;
    const [fbsy] = readF32(dv, off); off += 4;
    const [numLists] = readU32(dv, off); off += 4;

    const lists = [];
    for (let i = 0; i < numLists; i++) {
        const [numVtx] = readU32(dv, off); off += 4;
        const [numIdx] = readU32(dv, off); off += 4;
        const [numCmd] = readU32(dv, off); off += 4;

        const vtxBytes = numVtx * 20;
        const idxBytes = numIdx * 4;

        const vtxRaw = new Uint8Array(data.buffer, data.byteOffset + off, vtxBytes);
        off += vtxBytes;
        const idxRaw = new Uint8Array(data.buffer, data.byteOffset + off, idxBytes);
        off += idxBytes;

        const cmds = [];
        for (let j = 0; j < numCmd; j++) {
            const [cx] = readF32(dv, off); off += 4;
            const [cy] = readF32(dv, off); off += 4;
            const [cz] = readF32(dv, off); off += 4;
            const [cw] = readF32(dv, off); off += 4;
            const [texId] = readU64(dv, off); off += 8;
            const [idxOff] = readU32(dv, off); off += 4;
            const [vtxOff] = readU32(dv, off); off += 4;
            const [elemCount] = readU32(dv, off); off += 4;
            cmds.push({ cx, cy, cz, cw, texId, idxOff, vtxOff, elemCount });
        }

        lists.push({ vtxRaw, idxRaw, cmds });
    }

    return { dpx, dpy, dsw, dsh, fbsx, fbsy, numLists, lists };
}

let mediaLastFrame = null;
function renderFromParsed(frame) {
    mediaLastFrame = frame;
    const fbW = canvas.width;
    const fbH = canvas.height;
    // Scale the shared server layout to this canvas backing store.
    const scaleX = frame.dsw > 0 ? fbW / frame.dsw : (frame.fbsx || 1);
    const scaleY = frame.dsh > 0 ? fbH / frame.dsh : (frame.fbsy || 1);

    gl.viewport(0, 0, fbW, fbH);
    gl.scissor(0, 0, fbW, fbH);
    gl.clearColor(0.1, 0.1, 0.12, 1.0);
    gl.clear(gl.COLOR_BUFFER_BIT);

    gl.uniformMatrix4fv(u_proj_loc, false, ortho(frame.dpx, frame.dpx + frame.dsw, frame.dpy + frame.dsh, frame.dpy));
    gl.uniform1i(u_tex_loc, 0);

    for (let i = 0; i < frame.lists.length; i++) {
        const list = frame.lists[i];

        gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
        gl.bufferData(gl.ARRAY_BUFFER, list.vtxRaw, gl.DYNAMIC_DRAW);

        const stride = 20;
        gl.enableVertexAttribArray(a_pos);
        gl.vertexAttribPointer(a_pos, 2, gl.FLOAT, false, stride, 0);
        gl.enableVertexAttribArray(a_uv);
        gl.vertexAttribPointer(a_uv, 2, gl.FLOAT, false, stride, 8);
        gl.enableVertexAttribArray(a_color);
        gl.vertexAttribPointer(a_color, 4, gl.UNSIGNED_BYTE, true, stride, 16);

        gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, ibo);
        gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, list.idxRaw, gl.DYNAMIC_DRAW);

        for (const cmd of list.cmds) {
            if (cmd.elemCount === 0) continue;

            const clipX = Math.max(0, (cmd.cx - frame.dpx) * scaleX);
            const clipY = Math.max(0, fbH - (cmd.cw - frame.dpy) * scaleY);
            const clipW = Math.max(0, (cmd.cz - cmd.cx) * scaleX);
            const clipH = Math.max(0, (cmd.cw - cmd.cy) * scaleY);

            gl.scissor(clipX, clipY, clipW, clipH);

            const tex = textures.get(cmd.texId);
            gl.activeTexture(gl.TEXTURE0);
            gl.bindTexture(gl.TEXTURE_2D, tex || null);

            gl.vertexAttribPointer(a_pos, 2, gl.FLOAT, false, stride, cmd.vtxOff * stride);
            gl.vertexAttribPointer(a_uv, 2, gl.FLOAT, false, stride, cmd.vtxOff * stride + 8);
            gl.vertexAttribPointer(a_color, 4, gl.UNSIGNED_BYTE, true, stride, cmd.vtxOff * stride + 16);
            gl.drawElements(gl.TRIANGLES, cmd.elemCount, gl.UNSIGNED_INT, cmd.idxOff * 4);
        }
    }
}

const IMGUI_KEY_MAP = {
    "Tab": 512, "ArrowLeft": 513, "ArrowRight": 514, "ArrowUp": 515, "ArrowDown": 516,
    "PageUp": 517, "PageDown": 518, "Home": 519, "End": 520, "Insert": 521,
    "Delete": 522, "Backspace": 523, "Space": 524, "Enter": 525, "Escape": 526,
    "ControlLeft": 527, "ShiftLeft": 528, "AltLeft": 529, "MetaLeft": 530,
    "ControlRight": 531, "ShiftRight": 532, "AltRight": 533, "MetaRight": 534,
    "ContextMenu": 535,
    "Digit0": 536, "Digit1": 537, "Digit2": 538, "Digit3": 539, "Digit4": 540,
    "Digit5": 541, "Digit6": 542, "Digit7": 543, "Digit8": 544, "Digit9": 545,
    "KeyA": 546, "KeyB": 547, "KeyC": 548, "KeyD": 549, "KeyE": 550, "KeyF": 551,
    "KeyG": 552, "KeyH": 553, "KeyI": 554, "KeyJ": 555, "KeyK": 556, "KeyL": 557,
    "KeyM": 558, "KeyN": 559, "KeyO": 560, "KeyP": 561, "KeyQ": 562, "KeyR": 563,
    "KeyS": 564, "KeyT": 565, "KeyU": 566, "KeyV": 567, "KeyW": 568, "KeyX": 569,
    "KeyY": 570, "KeyZ": 571,
    "F1": 572, "F2": 573, "F3": 574, "F4": 575, "F5": 576, "F6": 577,
    "F7": 578, "F8": 579, "F9": 580, "F10": 581, "F11": 582, "F12": 583,
    "F13": 584, "F14": 585, "F15": 586, "F16": 587, "F17": 588, "F18": 589,
    "F19": 590, "F20": 591, "F21": 592, "F22": 593, "F23": 594, "F24": 595,
    "Quote": 596, "Comma": 597, "Minus": 598, "Period": 599,
    "Slash": 600, "Semicolon": 601, "Equal": 602,
    "BracketLeft": 603, "Backslash": 604, "BracketRight": 605,
    "Backquote": 606,
    "CapsLock": 607, "ScrollLock": 608, "NumLock": 609,
    "PrintScreen": 610, "Pause": 611,
    "Numpad0": 612, "Numpad1": 613, "Numpad2": 614, "Numpad3": 615,
    "Numpad4": 616, "Numpad5": 617, "Numpad6": 618, "Numpad7": 619,
    "Numpad8": 620, "Numpad9": 621, "NumpadDecimal": 622,
    "NumpadDivide": 623, "NumpadMultiply": 624, "NumpadSubtract": 625,
    "NumpadAdd": 626, "NumpadEnter": 627, "NumpadEqual": 628,
};

let connection = null;
let clientId = 0;
let pendingMouseMove = null;
let mouseMoveQueued = false;
let pendingSends = [];
let sendFlushQueued = false;

function flushSends() {
    sendFlushQueued = false;
    const messages = pendingSends;
    pendingSends = [];
    if (messages.length === 0 || clientId === 0 || !connection || connection.readyState !== 1) return;
    if (messages.length === 1) {
        connection.send(messages[0]);
        return;
    }

    // Oversized records cannot fit a batch's u16 payload length. Preserve
    // FIFO by sending this flush as individual messages (clipboard before V).
    if (messages.length > 0xffff || messages.some(msg => msg.byteLength - 5 > 0xffff)) {
        for (const msg of messages) connection.send(msg);
        return;
    }

    // 0x19 input batch: client_id u32, count u16, then repeated
    // { type u8, payload_len u16, payload bytes }. The shared client id is
    // omitted from each record to keep rapid pointer/key sequences compact.
    const count = Math.min(messages.length, 0xffff);
    let total = 7;
    for (let i = 0; i < count; i++) total += 3 + messages[i].byteLength - 5;
    const batch = new Uint8Array(total);
    const dv = new DataView(batch.buffer);
    batch[0] = 0x19;
    dv.setUint32(1, clientId, true);
    dv.setUint16(5, count, true);
    let off = 7;
    for (let i = 0; i < count; i++) {
        const msg = messages[i];
        const payload = msg.subarray(5);
        batch[off++] = msg[0];
        dv.setUint16(off, payload.byteLength, true); off += 2;
        batch.set(payload, off); off += payload.byteLength;
    }
    connection.send(batch);
}

function sendBytes(data) {
    // Client messages are not valid until the server's 0x06 assignment has
    // arrived. In particular, transport.onopen fires before that message, so
    // sending the initial resize from onopen used client id 0 and the server
    // silently ignored it.
    if (clientId !== 0 && connection && connection.readyState === 1) {
        pendingSends.push(new Uint8Array(data));
        if (!sendFlushQueued) {
            sendFlushQueued = true;
            queueMicrotask(flushSends);
        }
    }
}

function sendClipboardText(text) {
    const encoded = new TextEncoder().encode(text);
    const buf = new ArrayBuffer(5 + 4 + encoded.length);
    const dv = new DataView(buf);
    dv.setUint8(0, 0x18);
    dv.setUint32(1, clientId, true);
    dv.setUint32(5, encoded.length, true);
    new Uint8Array(buf).set(encoded, 9);
    sendBytes(buf);
}

function sendF32F32(type, a, b) {
    const buf = new ArrayBuffer(13);
    const dv = new DataView(buf);
    dv.setUint8(0, type);
    dv.setUint32(1, clientId, true);
    dv.setFloat32(5, a, true);
    dv.setFloat32(9, b, true);
    sendBytes(buf);
}

// Resize carries the browser's devicePixelRatio as a trailing scale so the
// server can lay out at native client scale. The optional scale defaults to 1.
function sendResize(w, h, scale) {
    const buf = new ArrayBuffer(17);
    const dv = new DataView(buf);
    dv.setUint8(0, 0x17);
    dv.setUint32(1, clientId, true);
    dv.setFloat32(5, w, true);
    dv.setFloat32(9, h, true);
    dv.setFloat32(13, scale, true);
    sendBytes(buf);
}

function sendU8(type, value) {
    const buf = new ArrayBuffer(6);
    const dv = new DataView(buf);
    dv.setUint8(0, type);
    dv.setUint32(1, clientId, true);
    dv.setUint8(5, value);
    sendBytes(buf);
}

function sendU16(type, value) {
    const buf = new ArrayBuffer(7);
    const dv = new DataView(buf);
    dv.setUint8(0, type);
    dv.setUint32(1, clientId, true);
    dv.setUint16(5, value, true);
    sendBytes(buf);
}

function sendU32(type, value) {
    const buf = new ArrayBuffer(9);
    const dv = new DataView(buf);
    dv.setUint8(0, type);
    dv.setUint32(1, clientId, true);
    dv.setUint32(5, value, true);
    sendBytes(buf);
}

function sendHelloAck(serverCapabilities = 0) {
    let capabilities = 1 << 0; // WebGL baseline
    if (typeof Worker !== "undefined" && typeof OffscreenCanvas !== "undefined") capabilities |= 1 << 1;
    if (globalThis.crossOriginIsolated && typeof SharedArrayBuffer !== "undefined") capabilities |= 1 << 2;
    if (navigator.gpu) capabilities |= 1 << 3;
    if (drawTransport) capabilities |= (1 << 4) | (1 << 5);
    if (drawTransport && (serverCapabilities & (1 << 7))) capabilities |= 1 << 7;
    const options = new URLSearchParams(globalThis.location?.hash?.slice(1) || "");
    const precision = options.get("draw-precision") ||
        (options.get("draw-quantized") === "0" ? "exact" : options.get("draw-quantized") === "1" ? "fine" : "integer");
    if (drawTransport && precision !== "exact") {
        const bit = precision === "fine" ? 6 : precision === "quarter" ? 8 : 9;
        if (serverCapabilities & (1 << bit)) capabilities |= 1 << bit;
        else if (bit === 9 && (serverCapabilities & (1 << 8))) capabilities |= 1 << 8;
        else if (serverCapabilities & (1 << 6)) capabilities |= 1 << 6;
    }
    if (drawTransport && (serverCapabilities & (1 << 10))) capabilities |= 1 << 10;
    const buf = new ArrayBuffer(9);
    const dv = new DataView(buf);
    dv.setUint8(0, 0x1a);
    dv.setUint32(1, clientId, true);
    dv.setUint32(5, capabilities, true);
    // Negotiation must be its own record; normal input may be microtask-batched.
    connection.send(buf);
}

function queueMouseMove(x, y) {
    pendingMouseMove = { x, y };
    if (mouseMoveQueued) return;
    mouseMoveQueued = true;
    queueMicrotask(flushMouseMove);
}

function flushMouseMove() {
    mouseMoveQueued = false;
    if (pendingMouseMove) {
        sendF32F32(0x10, pendingMouseMove.x, pendingMouseMove.y);
        pendingMouseMove = null;
    }
}

function resize() {
    const cssWidth = Math.max(1, Math.floor(canvas.clientWidth));
    const cssHeight = Math.max(1, Math.floor(canvas.clientHeight));
    const pixelRatio = Math.max(1, window.devicePixelRatio || 1);
    const backingWidth = Math.round(cssWidth * pixelRatio);
    const backingHeight = Math.round(cssHeight * pixelRatio);
    if (canvas.width !== backingWidth || canvas.height !== backingHeight) {
        canvas.width = backingWidth;
        canvas.height = backingHeight;
    }
    // ImGui coordinates and browser pointer coordinates are both CSS pixels.
    // Tell the server the CSS size and devicePixelRatio for input and rendering.
    sendResize(cssWidth, cssHeight, pixelRatio);
}

let reconnectDelay = 1000;
function connect() {
    drawTransport = false;
    drawDecoder?.reset();
    clientId = 0;
    pendingSends = [];
    sendFlushQueued = false;
    statusEl.textContent = "connecting (WebTransport)...";
    statusEl.className = "disconnected";
    try {
        connection = window.ImGuiTransport.create();
    } catch (error) {
        statusEl.textContent = error.message;
        return;
    }
    connection.binaryType = "arraybuffer";

    connection.onopen = () => {
        statusEl.textContent = "negotiating (WebTransport)...";
    };

    connection.onclose = () => {
        window.ImGuiBenchmark?.error("transport closed");
        clientId = 0;
        pendingSends = [];
        statusEl.textContent = "WebTransport disconnected - retrying...";
        statusEl.className = "disconnected";
        setTimeout(connect, reconnectDelay);
        reconnectDelay = Math.min(reconnectDelay * 2, 15000);
    };

    connection.onerror = (e) => { console.error("[imgui_quic] WebTransport error", e); };

    connection.onmessage = (evt) => {
        if (typeof evt.data === "string") return;
        const data = new Uint8Array(evt.data);
        if (data.length < 1) return;

        const msgType = data[0];
        switch (msgType) {
            case 0x06: {
                const dv = new DataView(data.buffer, data.byteOffset, data.byteLength);
                clientId = dv.getUint32(1, true);
                console.log("[imgui_quic] Assigned client ID:", clientId);
                break;
            }
            case 0x0a: {
                const magic = String.fromCharCode(data[1], data[2], data[3], data[4]);
                if (magic !== "IMGW" || data.length < 9) {
                    connection.close(1002, "unsupported imgui-quic protocol");
                    break;
                }
                const caps = new DataView(data.buffer,data.byteOffset,data.byteLength).getUint32(5,true);
                if (!drawDecoder || !(caps & (1 << 4))) {
                    connection.close(1002, "I/P draw transport required");
                    return;
                }
                statusEl.textContent = "connected (WebTransport)";
                statusEl.className = "connected";
                console.log("[imgui_quic]", statusEl.textContent);
                reconnectDelay = 1000;
                drawTransport = true;
                sendHelloAck(caps);
                // Synchronize client state after the protocol acknowledgement;
                // the server deliberately sends no state before negotiation.
                resize();
                setTimeout(()=>window.imgui_quicMedia?.report(),0);
                break;
            }
            case 0x1c: {
                if(data.length===6) window.imgui_quicMedia?.control(data[1],new DataView(data.buffer,data.byteOffset,data.byteLength).getFloat32(2,true));
                break;
            }
            case 0x02: {
                const dv = new DataView(data.buffer, data.byteOffset, data.byteLength);
                let off = 1;
                const id_lo = dv.getUint32(off, true); off += 4;
                const id_hi = dv.getUint32(off, true); off += 4;
                const id = id_lo + id_hi * 0x100000000;
                const w = dv.getUint32(off, true); off += 4;
                const h = dv.getUint32(off, true); off += 4;
                const pixLen = dv.getUint32(off, true); off += 4;
                const pixels = data.slice(off, off + pixLen);
                uploadTexture(id, w, h, pixels);
                break;
            }
            case 0x0d:
            case 0x0e:
            case 0x22:
            case 0x23:
            case 0x26:
            case 0x27:
            case 0x0f: {
                if (!drawTransport) break;
                try {
                    const bench = window.ImGuiBenchmark;
                    const decodeStart = bench ? performance.now() : undefined;
                    const decoded = drawDecoder.decode(data);
                    const decodeEnd = bench ? performance.now() : undefined;
                    const parsed = parseDrawLists(decoded.bytes);
                    if (parsed.lists.some(list => list.cmds.some(cmd =>
                        cmd.elemCount && cmd.texId !== 4294967294 && !textures.has(cmd.texId)))) throw Error("missing draw texture");
                    lastDpx = parsed.dpx; lastDpy = parsed.dpy;
                    lastDsw = parsed.dsw; lastDsh = parsed.dsh;
                    const sample = bench?.begin(decoded, evt, decodeStart, decodeEnd, gl);
                    try { renderFromParsed(parsed); }
                    finally { bench?.end(sample, gl); }
                    drawDecoder.commit(decoded);
                    sendDrawAck(decoded.id);
                } catch (error) {
                    window.ImGuiBenchmark?.error(error.message);
                    console.warn("[imgui_quic] requesting I frame:", error.message);
                    sendDrawAck(0);
                }
                break;
            }
            case 0x18: {
                const dv = new DataView(data.buffer, data.byteOffset, data.byteLength);
                const textLen = dv.getUint32(1, true);
                const textBytes = data.slice(5, 5 + textLen);
                const text = new TextDecoder().decode(textBytes);
                writeClipboardText(text);
                break;
            }
        }
    };
}

// Some browsers require a fresh user gesture for an async clipboard write.
// Keep the copied text accessible instead of silently losing it.
function showClipboardFallback(text) {
    let panel = document.getElementById("clipboard-fallback");
    if (!panel) {
        panel = document.createElement("div");
        panel.id = "clipboard-fallback";
        const label = document.createElement("label");
        label.textContent = "Clipboard access blocked. Copy the selected text:";
        const field = document.createElement("textarea");
        field.readOnly = true;
        field.setAttribute("aria-label", "Copied text");
        label.appendChild(field);
        const close = document.createElement("button");
        close.textContent = "Close";
        close.addEventListener("click", () => { panel.remove(); canvas.focus(); });
        panel.append(label, close);
        document.body.appendChild(panel);
    }
    const field = panel.querySelector("textarea");
    field.value = text;
    field.focus();
    field.select();
}
function writeClipboardText(text) {
    if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(text).catch(() => showClipboardFallback(text));
    } else {
        showClipboardFallback(text);
    }
}

function getCanvasPos(e) {
    const rect = canvas.getBoundingClientRect();
    const cssX = e.clientX - rect.left;
    const cssY = e.clientY - rect.top;
    // A frame from the previously active client may briefly be scaled to this
    // canvas. Hit-test in the coordinates of the frame actually on screen.
    const logicalWidth = lastDsw > 0 ? lastDsw : rect.width;
    const logicalHeight = lastDsh > 0 ? lastDsh : rect.height;
    return {
        x: cssX * logicalWidth / Math.max(1, rect.width),
        y: cssY * logicalHeight / Math.max(1, rect.height),
    };
}

canvas.addEventListener("pointermove", (e) => {
    const p = getCanvasPos(e);
    queueMouseMove(p.x, p.y);
});

// Pointers that went down on the canvas and have not come up yet. Pointer
// capture retargets the up event to the canvas, but some embedded browsers
// reject setPointerCapture (sandboxed iframes, partial pointer-event
// support); when that happens the window-level fallback below still delivers
// the release so ImGui never sees a stuck button.
const activePointers = new Set();
let pointerCaptureBroken = false;

canvas.addEventListener("pointerdown", (e) => {
    canvas.focus({ preventScroll: true });
    e.preventDefault();
    if (!pointerCaptureBroken) {
        try { canvas.setPointerCapture(e.pointerId); }
        catch (_) { pointerCaptureBroken = true; }
    }
    activePointers.add(e.pointerId);
    const p = getCanvasPos(e);
    flushMouseMove();
    // ImGui must see the click position before the button transition. Without
    // this, a first click (with no preceding move) targets the stale position.
    sendF32F32(0x10, p.x, p.y);
    sendU8(0x11, e.button);
});

function releasePointer(e) {
    // Capture delivers the up event to the canvas AND (via bubbling) to the
    // window; release exactly once per pointer.
    if (!activePointers.has(e.pointerId)) return;
    activePointers.delete(e.pointerId);
    const p = getCanvasPos(e);
    sendF32F32(0x10, p.x, p.y);
    sendU8(0x12, e.button);
    if (canvas.hasPointerCapture(e.pointerId)) {
        try { canvas.releasePointerCapture(e.pointerId); } catch (_) {}
    }
}

canvas.addEventListener("pointerup", releasePointer);
canvas.addEventListener("pointercancel", releasePointer);
// Click fallback for embedded browsers where pointer capture failed: the
// release lands on the window when the pointer comes up outside the canvas.
window.addEventListener("pointerup", releasePointer);
window.addEventListener("pointercancel", releasePointer);

canvas.addEventListener("wheel", (e) => {
    e.preventDefault();
    const dx = e.deltaX * 0.01, dy = -e.deltaY * 0.01;
    sendF32F32(0x13, dx, dy);
}, { passive: false });

canvas.addEventListener("contextmenu", (e) => e.preventDefault());

const keysDown = new Set();
const modifierKeys = { ctrl: 4096, shift: 8192, alt: 16384, super: 32768 };
const modifierDown = new Set();
function setModifier(key, down) {
    if (modifierDown.has(key) === down) return;
    if (down) modifierDown.add(key); else modifierDown.delete(key);
    sendU16(down ? 0x14 : 0x15, key);
}
function syncModifiers(e) {
    // The native server uses Ctrl shortcuts. Command also maps to Ctrl so
    // macOS browser users get the same editing actions.
    setModifier(modifierKeys.ctrl, !!(e.ctrlKey || e.metaKey));
    setModifier(modifierKeys.shift, !!e.shiftKey);
    setModifier(modifierKeys.alt, !!e.altKey);
    // Do not also set Super: Ctrl+Super+C is a different ImGui chord.
    setModifier(modifierKeys.super, false);
}
function isClipboardKey(e) {
    return (e.ctrlKey || e.metaKey) && !e.altKey && ["KeyC", "KeyX", "KeyV"].includes(e.code);
}

document.addEventListener("keydown", (e) => {
    // Allow native controls used for clipboard permission fallback.
    if (e.target instanceof HTMLInputElement || e.target instanceof HTMLTextAreaElement ||
        e.target?.closest?.("#clipboard-fallback, #media-panel, #media-open")) return;
    syncModifiers(e);
    const key = IMGUI_KEY_MAP[e.code];
    if (key === undefined) return;
    // Let browser clipboard events handle shortcuts and Edit-menu actions;
    // paste carries the data without an async permission/read race.
    if (isClipboardKey(e)) return;
    e.preventDefault();
    if (!keysDown.has(key)) {
        keysDown.add(key);
        sendU16(0x14, key);
    }
    if (e.key && e.key.length === 1 && !e.ctrlKey && !e.altKey && !e.metaKey) {
        const cp = e.key.codePointAt(0);
        sendU32(0x16, cp);
    }
});

function clipboardShortcut(key) {
    const ctrlWasDown = modifierDown.has(modifierKeys.ctrl);
    setModifier(modifierKeys.ctrl, true);
    sendU16(0x14, key);
    sendU16(0x15, key);
    if (!ctrlWasDown) setModifier(modifierKeys.ctrl, false);
}
for (const [eventName, key] of [["copy", IMGUI_KEY_MAP.KeyC], ["cut", IMGUI_KEY_MAP.KeyX]]) {
    document.addEventListener(eventName, (e) => {
        if (e.target instanceof HTMLInputElement || e.target instanceof HTMLTextAreaElement ||
            e.target?.closest?.("#clipboard-fallback, #media-panel, #media-open")) return;
        e.preventDefault();
        // Also covers browser Edit menu actions which emit clipboard events
        // without any keyboard events. The server replies with selected text.
        clipboardShortcut(key);
    });
}

document.addEventListener("paste", (e) => {
    if (e.target instanceof HTMLInputElement || e.target instanceof HTMLTextAreaElement ||
        e.target?.closest?.("#clipboard-fallback, #media-panel, #media-open")) return;
    if (!e.clipboardData) return;
    e.preventDefault();
    const text = e.clipboardData.getData("text/plain");
    if (!text) return;
    sendClipboardText(text);
    // Use ImGui's real paste action: preserves selection replacement,
    // multiline filtering, and a single undo operation.
    clipboardShortcut(IMGUI_KEY_MAP.KeyV);
});

document.addEventListener("keyup", (e) => {
    if (e.target instanceof HTMLInputElement || e.target instanceof HTMLTextAreaElement ||
        e.target?.closest?.("#clipboard-fallback, #media-panel, #media-open")) return;
    syncModifiers(e);
    const key = IMGUI_KEY_MAP[e.code];
    if (key !== undefined) {
        e.preventDefault();
        keysDown.delete(key);
        sendU16(0x15, key);
    }
});

function releaseKeyboardInput() {
    for (const key of keysDown) {
        sendU16(0x15, key);
    }
    keysDown.clear();
    for (const key of [...modifierDown]) setModifier(key, false);
}
window.addEventListener("blur", releaseKeyboardInput);
window.addEventListener("imgui-quic-release-input", releaseKeyboardInput);

window.addEventListener("resize", resize);
if (window.ResizeObserver) {
    new ResizeObserver(resize).observe(canvas);
}

window.imgui_quicMedia?.attach({
    upload(id,source) {
        const max=gl.getParameter(gl.MAX_TEXTURE_SIZE);
        if((source.videoWidth||source.naturalWidth)>max || (source.videoHeight||source.naturalHeight)>max)
            throw new Error('file dimensions exceed this GPU limit');
        let tex=textures.get(id);
        if(!tex){tex=gl.createTexture();textures.set(id,tex);}
        gl.bindTexture(gl.TEXTURE_2D,tex);
        gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_S,gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_T,gl.CLAMP_TO_EDGE);
        gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,gl.RGBA,gl.UNSIGNED_BYTE,source);
    },
    remove(id){const tex=textures.get(id);if(tex)gl.deleteTexture(tex);textures.delete(id);},
    redraw(){if(mediaLastFrame)renderFromParsed(mediaLastFrame);},
    report(m){
        const message=new TextEncoder().encode(m.message).subarray(0,1023);
        const data=new Uint8Array(33+message.length),v=new DataView(data.buffer);
        data[0]=0x1b;v.setUint32(1,clientId,true);
        v.setUint32(5,m.kind|(m.paused?4:0)|(m.muted?8:0)|(m.error?16:0),true);
        v.setUint32(9,m.width,true);v.setUint32(13,m.height,true);
        v.setFloat32(17,m.position,true);v.setFloat32(21,m.duration,true);v.setFloat32(25,m.volume,true);
        v.setUint32(29,message.length,true);data.set(message,33);sendBytes(data);
    }
});
resize();
connect();
