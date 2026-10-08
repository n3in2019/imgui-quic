#!/usr/bin/env python3
"""Real native side-channel test; no QUIC/browser required, no latency claims."""
import os
import select
import subprocess
import sys
import time

env = {k: v for k, v in os.environ.items()
       if not k.startswith(('IMGUI_QUIC_', 'IMGW_BENCH_'))}
env['IMGW_BENCH_CLOCK_STDIO'] = '1'
host = subprocess.Popen([sys.argv[1], 'moving', '30'], env=env,
                        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                        stderr=subprocess.DEVNULL)
try:
    stamps = []
    for command in (b'c', b'xc', b'c'):
        host.stdin.write(command)
        host.stdin.flush()
        assert select.select([host.stdout], [], [], 3)[0], 'clock probe timed out'
        stamps.append(int(host.stdout.readline()))
    assert 0 < stamps[0] < stamps[1] < stamps[2], stamps
    # Keep stdin open: shutdown must not wait forever for the next command.
    before = time.monotonic()
    host.terminate()
    assert host.wait(timeout=2) == 0
    assert time.monotonic() - before < 2
    print('native clock: monotonic replies, ignored input, bounded shutdown PASS')
finally:
    if host.poll() is None:
        host.kill()
        host.wait()
