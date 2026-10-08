// All durations in ms. No cross-clock subtraction without a probe interval.
export function percentile(values, p) {
    const xs = values.filter(Number.isFinite).sort((a,b) => a-b);
    return xs.length ? xs[Math.ceil((xs.length-1)*p)] : null;
}
export function clockInterval(probes, at, driftPpm = 100, resolutionMs = 1) {
    if (!probes.length) throw Error('no clock probes');
    let low = -Infinity, high = Infinity;
    for (const p of probes) {
        if (!(p.end_ms >= p.start_ms) || !/^\d+$/.test(p.native_ns)) throw Error('invalid clock probe');
        const native = Number(BigInt(p.native_ns)) / 1e6;
        const drift = Math.max(Math.abs(at-p.start_ms), Math.abs(at-p.end_ms))*driftPpm/1e6;
        low = Math.max(low, p.start_ms-native-resolutionMs-drift);
        high = Math.min(high, p.end_ms-native+resolutionMs+drift);
    }
    if (!(low <= high)) throw Error('inconsistent clock probes / drift budget exceeded');
    return {low_ms: low, high_ms: high, uncertainty_ms: (high-low)/2};
}
export function summarize(snapshot, probes, options) {
    const rows = snapshot.rows, failures = [...snapshot.errors];
    if (snapshot.visibility !== 'visible') failures.push('page hidden');
    if (rows.length < options.minFrames) failures.push('insufficient frames');
    let previous = 0;
    for (const r of rows) {
        if (r.id <= previous) failures.push('nonmonotonic frame ID');
        previous = r.id;
        if (![r.receive_ms,r.decode_start_ms,r.decode_end_ms,r.submit_start_ms,r.submit_end_ms,
            r.raf_callback_ms,r.next_raf_proxy_ms].every(Number.isFinite)) failures.push('incomplete sample');
        if (!(r.receive_ms <= r.decode_start_ms && r.decode_start_ms <= r.decode_end_ms &&
              r.decode_end_ms <= r.submit_start_ms && r.submit_start_ms <= r.submit_end_ms &&
              r.submit_end_ms <= r.raf_callback_ms && r.raf_callback_ms <= r.next_raf_proxy_ms))
            failures.push('invalid stage ordering');
        try {
            const clock = clockInterval(probes, r.receive_ms, options.driftPpm, options.resolutionMs);
            r.clock_uncertainty_ms = clock.uncertainty_ms;
            const gen = Number(BigInt(r.native_ns))/1e6;
            for (const [label, endpoint] of [['receive',r.receive_ms],['submit',r.submit_end_ms],
                                            ['raf_proxy',r.next_raf_proxy_ms]]) {
                const stageClock = clockInterval(probes, endpoint, options.driftPpm, options.resolutionMs);
                r.clock_uncertainty_ms = Math.max(r.clock_uncertainty_ms, stageClock.uncertainty_ms);
                r[`generation_to_${label}_ms`] = endpoint-gen-(stageClock.low_ms+stageClock.high_ms)/2;
                r[`generation_to_${label}_lower_ms`] = endpoint-gen-stageClock.high_ms;
                r[`generation_to_${label}_upper_ms`] = endpoint-gen-stageClock.low_ms;
            }
            if (r.generation_to_receive_upper_ms < 0) failures.push('generation after receipt outside error bound');
        } catch (e) { failures.push(e.message); }
    }
    const durationMs = snapshot.stop_ms-snapshot.start_ms;
    const fps = durationMs > 0 ? rows.length*1000/durationMs : 0;
    if (options.minFps !== undefined && fps < options.minFps) failures.push('throughput budget');
    const stats = values => ({n: values.filter(Number.isFinite).length,
        p50: percentile(values,.5), p95: percentile(values,.95), max: percentile(values,1)});
    const metrics = {
        receive_to_decode_start_ms: stats(rows.map(r=>r.decode_start_ms-r.receive_ms)),
        submit_to_raf_callback_ms: stats(rows.map(r=>r.raf_callback_ms-r.submit_end_ms)),
        decode_ms: stats(rows.map(r=>r.decode_end_ms-r.decode_start_ms)),
        parse_and_prepare_ms: stats(rows.map(r=>r.submit_start_ms-r.decode_end_ms)),
        cpu_submit_ms: stats(rows.map(r=>r.submit_end_ms-r.submit_start_ms)),
        gpu_elapsed_ms: stats(rows.map(r=>r.gpu_elapsed_ms)),
        generation_to_receive_ms: stats(rows.map(r=>r.generation_to_receive_ms)),
        generation_to_submit_ms: stats(rows.map(r=>r.generation_to_submit_ms)),
        generation_to_raf_proxy_ms: stats(rows.filter(r=>r.superseded_before_proxy===false).map(r=>r.generation_to_raf_proxy_ms)),
        generation_to_raf_proxy_upper_ms: stats(rows.filter(r=>r.superseded_before_proxy===false).map(r=>r.generation_to_raf_proxy_upper_ms)),
        clock_uncertainty_ms: stats(rows.map(r=>r.clock_uncertainty_ms))
    };
    if (metrics.clock_uncertainty_ms.max > options.maxUncertaintyMs) failures.push('clock uncertainty budget');
    if (metrics.cpu_submit_ms.p95 > options.maxSubmitMs) failures.push('CPU submit p95 budget');
    if (options.maxProxyMs !== null && (metrics.generation_to_raf_proxy_upper_ms.p95 === null ||
        metrics.generation_to_raf_proxy_upper_ms.p95 > options.maxProxyMs)) failures.push('RAF proxy p95 budget');
    return {status: failures.length ? 'FAIL' : 'PASS', failures: [...new Set(failures)],
        duration_ms: durationMs, fps, frames: rows.length, superseded_before_proxy: rows.filter(r=>r.superseded_before_proxy).length, metrics};
}
