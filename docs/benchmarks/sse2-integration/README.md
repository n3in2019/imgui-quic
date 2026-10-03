# Production SSE2 quantization

Coordinate and UV packing uses SSE2 on compiler targets defining `__SSE2__`.
The implementation pairs x/y and u/v while preserving double-precision
calculations, signed half-unit bias, truncation, float32 reconstruction checks,
range validation and whole-frame fallback. Other architectures retain scalar
packing. `IMGUI_QUIC_FORCE_SCALAR_QUANTIZATION=ON` selects the scalar path for
validation. Public interfaces, precision modes, compression selection and
client decoding are unchanged.

Five randomized, interleaved trials of 900 frames per scene plus 30 warm-up
frames compare the integrated production encoder with the same source built in forced-scalar mode on
one local x86-64 CPU. Serialization, quantization, allocation, compression
selection and baseline ownership updates are inside the timer; ImGui layout,
State publishing, native QUIC and browser rendering are outside it.

| Scene | Scalar µs | SSE2 µs | Encoding CPU reduction | Unchanged bytes/frame |
|---|---:|---:|---:|---:|
| static | 18.205 | 9.521 | 47.7% | 13.000 |
| moving | 18.324 | 9.691 | 47.1% | 35.894 |
| dynamic | 28.378 | 18.530 | 34.7% | 939.047 |
| plots | 386.140 | 330.107 | 14.5% | 8009.670 |
| tables | 193.196 | 156.309 | 19.1% | 4703.479 |
| topology | 169.599 | 153.728 | 9.4% | 13516.423 |
| dense | 499.800 | 359.315 | 28.1% | 57104.299 |

Values are medians of per-trial means. [Raw results](results.json) contain all
70 trials, host/compiler details, dependency pins and binary/source hashes.
Static/moving results force encoding every frame, so their percentages do not
represent idle server CPU reductions. Whole-server gains must be assessed
separately; the earlier [native server CPU evaluation](../server-cpu/README.md)
measured about 9% lower CPU on heavy scenes and little resolved gain on light
workloads. ARM performance remains unmeasured and ARM uses the scalar path.

Validation completed:

- All seven production CTest suites and nine frontend regression scripts passed.
- Exact, integer, quarter and fine fixtures across seven scenes produced
  identical SHA-256 hashes for 3,360 frames, including warm-up.
- The quantization CTest compares the enabled backend against an independently
  compiled scalar implementation. It covers half ties and adjacent floats,
  conversion bounds, NaN/Inf, signed zero, UV endpoints, large/tiny origins and
  300,000 differential cases (86,871 packed outputs, 213,129 fallbacks).
- AddressSanitizer/UndefinedBehaviorSanitizer draw-protocol and quantization
  tests passed. A separate forced-scalar Release build passed all six core
  CTest suites (the development launcher was not built in that configuration).
- Native QUIC interoperability passed integer, quarter and fine modes,
  including input, ACK ordering, resource barriers, recovery and bounded queues.

## Reproduce

Prepare comparison dependencies using the
[benchmark guide](../../../tools/benchmarks/comparison/README.md), then run:

```bash
cmake -S tools/benchmarks/comparison -B build/sse2-reference -DCMAKE_BUILD_TYPE=Release -DIMGUI_QUIC_FORCE_SCALAR_QUANTIZATION=ON
cmake --build build/sse2-reference -j6
cmake --build build/comparison/build -j6
python3 tools/benchmarks/comparison/cpu.py --before build/sse2-reference/codec --after build/comparison/build/codec --before-source src/draw_protocol.cpp --output build/sse2-integration/results.json
```

Run timing without concurrent builds or other benchmarks. The comparator stops
on any fixture mismatch before collecting CPU measurements.
