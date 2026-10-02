# Native WebTransport benchmarks

The benchmark runs the native QUICHE host and an independent Python draw decoder
inside an isolated network namespace. It measures exact I/P geometry at 30 FPS
for a moving window and dynamic text. It does not measure browser WebGL/compositor time.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DIMGUI_QUIC_BUILD_BENCHMARKS=ON -DIMGUI_QUIC_BUILD_EXAMPLES=ON
cmake --build build -j 6
python3 -m venv build/webtransport_venv
build/webtransport_venv/bin/pip install -r tools/benchmarks/requirements.txt
./build/examples/imgui_quic_dev --prepare-only
IMGW_BENCH_HOST_NETNS="$(readlink /proc/self/ns/net)" unshare --user --map-root-user --net \
  build/webtransport_venv/bin/python tools/benchmarks/transport.py \
  --seconds 12 --trials 2 --output build/benchmarks/transport/results.json
python3 tools/benchmarks/summarize_transport.py build/benchmarks/transport/results.json
```

Requires Linux user/network namespaces, iproute2 and ethtool. The harness refuses
to shape the caller's network namespace. Only UDP port 19443 is shaped; both
directions share the configured bandwidth budget. The benchmark native process
has no TCP listener or static assets.

Use `--quick` for local-only runs, `--scenes moving` or `--scenes dynamic` for a
single workload, and `--host` to select a saved benchmark executable. Full runs
cover local, 100 ms RTT/1% loss, 300 ms RTT/3% loss and 100 ms RTT/5% loss.
Impaired profiles use 2 Mbit/s with jitter; trials use different netem seeds.
Failed cases remain in the JSON and make the command exit unsuccessfully.

The first frame and a one-second warmup precede measurement. Three vertex colors
encode native monotonic generation time and consumed pointer sequence. Pointer
updates arrive at 10 Hz. Frame age ends at client decode/ACK; input latency excludes
skipped updates, so observed/sent counts are reported alongside it. CPU is percent
of one logical CPU across native threads; RSS is final resident memory, not peak.
Context switches sum voluntary and involuntary switches across native threads.

Use `perf record -e cpu-clock:u -F 1999 --call-graph dwarf,16384 -p PID -o cpu.data`
to sample an active benchmark host. Keep profiled runs separate from clean latency
measurements and preserve the matching binaries. Do not infer hardware cache or
kernel costs from userspace software-event samples.
