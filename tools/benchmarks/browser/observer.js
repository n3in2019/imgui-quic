/* Opt-in via Playwright addInitScript; production pages do not load this file. */
(() => {
    'use strict';
    const rows = [], errors = [];
    let active = false, latest = null, ext, initialized = false, disjoint = false;
    let startMs = null, stopMs = null;
    document.addEventListener("visibilitychange", () => { if (active && document.hidden) errors.push("page hidden during measurement"); });
    const pending = [];
    function poll(gl) {
        if (!ext) return;
        if (gl.getParameter(ext.GPU_DISJOINT_EXT)) {
            disjoint = true;
            for (const row of rows) { row.gpu_elapsed_ms = null; row.gpu_status = 'disjoint'; }
        }
        for (let i = pending.length - 1; i >= 0; --i) {
            const {query, row} = pending[i];
            if (disjoint || ext.getQueryObjectEXT(query, ext.QUERY_RESULT_AVAILABLE_EXT)) {
                if (!disjoint) {
                    row.gpu_elapsed_ms = ext.getQueryObjectEXT(query, ext.QUERY_RESULT_EXT) / 1e6;
                    row.gpu_status = 'measured';
                } else row.gpu_status = 'disjoint';
                ext.deleteQueryEXT(query); pending.splice(i, 1);
            }
        }
    }
    window.ImGuiBenchmark = {
        rows, errors,
        start() { rows.length = 0; startMs = performance.now(); active = true; },
        stop() { stopMs = performance.now(); active = false; },
        error(message) { errors.push(String(message)); },
        begin(decoded, evt, decodeStart, decodeEnd, gl) {
            latest = decoded.id;
            if (!active) return null;
            if (!initialized) {
                ext = gl.getExtension('EXT_disjoint_timer_query');
                if (ext && !ext.getQueryEXT(ext.TIME_ELAPSED_EXT, ext.QUERY_COUNTER_BITS_EXT)) ext = null;
                initialized = true;
            }
            poll(gl);
            if (rows.length >= 100000) throw Error('benchmark sample limit');
            const bytes = decoded.bytes, v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
            if (bytes.length < 101 || v.getUint32(25, true) < 1 || v.getUint32(29, true) < 3)
                throw Error('missing benchmark marker');
            const nativeNs = BigInt(v.getUint32(57, true)) | (BigInt(v.getUint32(77, true)) << 32n);
            const row = {id: decoded.id, native_ns: nativeNs.toString(), path: evt.path,
                receive_ms: evt.receivedAt, decode_start_ms: decodeStart, decode_end_ms: decodeEnd,
                submit_start_ms: null, submit_end_ms: null, raf_callback_ms: null, raf_timestamp_ms: null,
                next_raf_proxy_ms: null, superseded_before_raf: null, superseded_before_proxy: null,
                gpu_elapsed_ms: null, gpu_status: ext ? 'pending' : 'unsupported',
                compositor_present_ms: null};
            rows.push(row);
            let query = null;
            if (ext && !disjoint && pending.length < 256) {
                query = ext.createQueryEXT();
                if (query) ext.beginQueryEXT(ext.TIME_ELAPSED_EXT, query);
                else row.gpu_status = "allocation_failed";
            } else if (ext) row.gpu_status = disjoint ? 'disjoint' : 'query_budget';
            row.submit_start_ms = performance.now();
            return {row, query};
        },
        end(sample, gl) {
            if (!sample) return;
            const {row, query} = sample;
            row.submit_end_ms = performance.now();
            if (query) { ext.endQueryEXT(ext.TIME_ELAPSED_EXT); pending.push({row, query}); }
            requestAnimationFrame(timestamp => {
                row.raf_callback_ms = performance.now(); row.raf_timestamp_ms = timestamp;
                row.superseded_before_raf = latest !== row.id;
                poll(gl);
                requestAnimationFrame(() => {
                    row.next_raf_proxy_ms = performance.now();
                    row.superseded_before_proxy = latest !== row.id;
                    poll(gl);
                });
            });
        },
        snapshot(gl) {
            if (gl) {
                poll(gl);
                if (gl.isContextLost()) errors.push("WebGL context lost");
                if (gl.getError() !== gl.NO_ERROR) errors.push("WebGL error");
            }
            return {rows, errors, start_ms: startMs, stop_ms: stopMs, gpu_timer_supported: !!ext, gpu_disjoint_observed: disjoint,
                visibility: document.visibilityState, time_origin_ms: performance.timeOrigin};
        }
    };
})();
