# Native server CPU usage

Whole-server measurements compare scalar and
[SSE2 quantization](../sse2-integration/README.md). Rendering, serialization,
quantization, codec selection, state publishing and native QUIC are included.
The recorded hosts differ only in their draw encoding backend; rendering and
QUIC use the same native implementation.

CPU percentage is user plus system process CPU time, summed across threads, divided
by wall time: **100% means one logical CPU fully occupied**, rather than the whole
24-logical-CPU machine. Three randomized/interleaved trials per case measure eight
seconds after a two-second warmup. The server is pinned to CPU 0 and independent
Python/aioquic decode/ACK receivers to CPU 1, which are different physical cores on
this AMD Ryzen AI 9 HX 470. Frequency boosting and unrelated system activity are not
controlled. All receivers negotiate integer coordinates, LZ4 and planar frames.

| Scene | Clients | Stock CPU | SSE2 CPU | Decoded FPS stock / SSE2 | Sampled peak RSS MiB stock / SSE2 |
|---|---:|---:|---:|---:|---:|
| dynamic | 1 | 0.87% | 0.87% | 30.03 / 30.03 | 12.22 / 12.18 |
| dynamic | 4 | 1.36% | 1.24% | 30.02 / 30.03 | 12.53 / 12.41 |
| dynamic | 8 | 1.98% | 2.00% | 30.00 / 30.00 | 12.86 / 12.86 |
| plots | 1 | 2.85% | 2.59% | 29.47 / 29.35 | 15.87 / 15.93 |
| dense | 1 | 4.23% | 3.86% | 29.97 / 29.99 | 18.30 / 18.34 |

Values are medians across trials; multi-client FPS is the mean across receivers
before taking the trial median. Curves show about 9% lower whole-server CPU and
dense geometry about 9%, smaller than the encoder-only gains because quantization
is only part of the process cost. The one-client text case shows no resolved
improvement, and the eight-client difference is within run variation. Three short
trials are insufficient to claim a statistically established gain for small
differences. The results do not show a universal whole-server CPU win.

| Scene | Clients | Variant | CPU range across trials |
|---|---:|---|---:|
| dynamic | 1 | stock | 0.87–0.87% |
| dynamic | 1 | sse2 | 0.87–0.99% |
| dynamic | 4 | stock | 1.23–1.48% |
| dynamic | 4 | sse2 | 1.24–1.36% |
| dynamic | 8 | stock | 1.87–2.22% |
| dynamic | 8 | sse2 | 1.98–2.10% |
| plots | 1 | stock | 2.75–2.96% |
| plots | 1 | sse2 | 2.49–2.85% |
| dense | 1 | stock | 4.10–4.35% |
| dense | 1 | sse2 | 3.73–3.98% |

[Raw results](results.json) contain every trial, source/binary hashes, host details,
per-client FPS, payload, Python receive/decode time, and sampled server RSS. CPU
counters have 10 ms resolution on this host, corresponding to approximately 0.12
percentage points per eight-second trial. RSS is sampled every 100 ms and may miss
short peaks. Receiver CPU is excluded from the server percentage.

The workload produces timestamp marker vertices every frame, so even text geometry
has changing bytes. These measurements describe an active 30 FPS stream rather
than a truly idle server or geometry-suppressed application. The target is local
loopback QUIC inside an isolated network namespace, without network shaping or
active pointer input. Asset serving, browser JavaScript/WebGL rendering, browser
presentation latency and ARM CPU usage are outside this measurement. Python
receivers ACK decoded frames, so the FPS column describes decoded delivery, not
browser presentation. The curves receiver runs near its decode capacity; retain
its observed FPS alongside CPU rather than assuming perfect 30 FPS pacing.

## Reproduce

Build the native server benchmark, prepare development credentials, and install
the independent receiver requirements as described in the
[transport benchmark guide](../../../tools/benchmarks/README.md). Prepare the
forced-scalar native build using the same project source.
From the project root:

```bash
cmake -S . -B build/sse2-scalar -DCMAKE_BUILD_TYPE=Release -DIMGUI_QUIC_NATIVE_QUIC=ON -DIMGUI_QUIC_BUILD_BENCHMARKS=ON -DIMGUI_QUIC_FORCE_SCALAR_QUANTIZATION=ON
cmake --build build/sse2-scalar -j6
IMGW_BENCH_HOST_NETNS="$(readlink /proc/self/ns/net)" unshare --user --map-root-user --net build/webtransport_venv/bin/python tools/benchmarks/comparison/server_cpu.py --seconds 8 --trials 3 --output build/server-cpu/results.json
```

The runner refuses the host network namespace and brings up only its private
loopback interface. Each host and its receivers are stopped between trials. Avoid
concurrent builds and other benchmark runs. Production SIMD validation is recorded in the
[quantization report](../sse2-integration/README.md).
