# Packed draw encoder CPU evaluation

The encoder uses fixed 12/20-byte lane loops with separate baseline-prefix and
zero-extension ranges. Coordinate reconstruction checks multiply by a precomputed
power-of-two reciprocal. These changes preserve candidate selection, precision,
wire bytes and client decoding cost.

Results below are median per-trial mean encoding times over five randomized,
interleaved trials of 900 measured frames plus 30 warm-up frames, pinned to one
local x86-64 CPU. Encoding includes snapshot serialization, quantization,
allocation, codec selection and baseline updates; ImGui layout is outside the
timer. The mode is the production integer-coordinate/planar selector. These are
encoder measurements, rather than whole-server CPU or browser frame rates.

| Scene | Before µs | After µs | CPU reduction | Unchanged bytes/frame |
|---|---:|---:|---:|---:|
| static | 19.480 | 18.201 | 6.6% | 13.000 |
| moving | 19.555 | 18.356 | 6.1% | 35.894 |
| dynamic | 29.778 | 28.425 | 4.5% | 939.047 |
| plots | 492.171 | 390.421 | 20.7% | 8009.670 |
| tables | 236.275 | 195.933 | 17.1% | 4703.479 |
| topology | 198.081 | 170.906 | 13.7% | 13516.423 |
| dense | 501.403 | 486.881 | 2.9% | 57104.299 |

The [raw results](results.json) record all 70 trials, binary/source/harness hashes,
compiler, host and dependency revisions. Four precision modes across seven scenes
produced identical fixture hashes for 3,360 frames, including warm-up frames.
The unchanged wire output retains the traffic measurements from the
[packed transport report](../packed-live/README.md).

`perf record` on 5,000 curve frames located the main costs in residual generation,
LZ4 and quantization. The encoder's self-sample share fell from 38.84% to 26.86%;
whole-process sampled cycles fell from approximately 13.68 billion to 10.86
billion. These separate sampled runs support the hotspot diagnosis; the randomized
encoder timings above measure the gain. See [profile summaries](perf.json).
LZ4 and quantization still account for substantial CPU. ARM gains and changes in
whole-server CPU have not been measured in this evaluation.

Validation: Release build, all six CTest suites, and draw I/P, compressed draw,
three quantization modes, planar draw and WebTransport browser regressions passed.
The native integer/planar QUIC interoperability suite also passed, including
presentation ACKs, recovery, input scheduling and resource barriers.

## Reproduce

Prepare pinned comparison dependencies as described in the
[benchmark guide](../../../tools/benchmarks/comparison/README.md). The
[optimization patch](optimization.patch) records the changes against the measured
reference. From the project root, reconstruct its source and build both binaries:

```bash
mkdir -p build/comparison/cpu-baseline
patch --reverse --output build/comparison/cpu-baseline/draw_protocol.cpp src/draw_protocol.cpp < docs/benchmarks/packed-cpu/optimization.patch
cmake -S tools/benchmarks/comparison -B build/comparison/cpu-reference -DCMAKE_BUILD_TYPE=Release -DDRAW_SOURCE="$PWD/build/comparison/cpu-baseline/draw_protocol.cpp"
cmake --build build/comparison/cpu-reference -j6
cmake --build build/comparison/build -j6
python3 tools/benchmarks/comparison/cpu.py --before build/comparison/cpu-reference/codec --after build/comparison/build/codec --before-source build/comparison/cpu-baseline/draw_protocol.cpp --output build/comparison/cpu-results.json
```

Run CPU benchmarks separately from builds and other timing runs. `cpu.py` verifies
fixture equality before measuring; a wire mismatch stops the comparison.
