#!/usr/bin/env python3
"""Compare two codec binaries on identical fixtures and randomized CPU trials."""
import argparse, hashlib, json, os, platform, random, statistics, subprocess
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--before', type=Path, required=True)
p.add_argument('--after', type=Path, required=True)
p.add_argument('--before-source', type=Path, required=True)
p.add_argument('--frames', type=int, default=900)
p.add_argument('--trials', type=int, default=5)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
if a.frames < 1 or a.trials < 1:
    p.error('frames and trials must be positive')
root = Path(__file__).resolve().parents[3]
cpu = min(os.sched_getaffinity(0))
os.sched_setaffinity(0, {cpu})
scenes = ['static', 'moving', 'dynamic', 'plots', 'tables', 'topology', 'dense']
binaries = {'before': a.before.resolve(), 'after': a.after.resolve()}
def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
def fixtures(binary, algorithm, scene):
    with subprocess.Popen([str(binary), algorithm, scene, '90', 'fixtures'], stdout=subprocess.PIPE) as child:
        h = hashlib.sha256()
        while block := child.stdout.read(1024*1024):
            h.update(block)
        if child.wait():
            raise RuntimeError('fixture generation failed')
        return h.hexdigest()
checks = []
for precision in ['', '-q1', '-q4', '-q16']:
    algorithm = 'imgui-quic' + precision + '-adaptive-live'
    for scene in scenes:
        hashes = {label: fixtures(binary, algorithm, scene) for label, binary in binaries.items()}
        if hashes['before'] != hashes['after']:
            raise RuntimeError(f'wire changed: {algorithm} {scene}')
        checks.append({'algorithm': algorithm, 'scene': scene, 'frames_including_warmup': 120, 'sha256': hashes['after']})
jobs = [(trial, scene, label) for trial in range(a.trials) for scene in scenes for label in binaries]
random.Random(42).shuffle(jobs)
results = []
for trial, scene, label in jobs:
    row = json.loads(subprocess.check_output([str(binaries[label]), 'imgui-quic-q1-adaptive-live', scene, str(a.frames)], text=True))
    row.update(trial=trial, version=label)
    results.append(row)
report = {'frames': a.frames, 'trials': a.trials, 'warmup_frames': 30, 'random_seed': 42,
          'cpu_affinity': cpu, 'system': platform.platform(),
          'cpuinfo': Path('/proc/cpuinfo').read_text().split('\n\n')[0],
          'compiler': subprocess.check_output(['c++', '--version'], text=True).splitlines()[0],
          'binary_sha256': {label: digest(binary) for label, binary in binaries.items()},
          'before_source_sha256': digest(a.before_source),
          'after_source_sha256': digest(root/'src/draw_protocol.cpp'),
          'harness_sha256': {name: digest(Path(__file__).with_name(name)) for name in ['codec.cpp', 'scenes.hpp']},
          'dependency_commits': {name: subprocess.check_output(['git', '-C', str(root/'third_party'/name), 'rev-parse', 'HEAD'], text=True).strip() for name in ['imgui', 'lz4']},
          'fixture_checks': checks, 'results': results}
a.output.parent.mkdir(parents=True, exist_ok=True)
a.output.write_text(json.dumps(report, indent=2)+'\n')
print('scene before_us after_us reduction_percent bytes_per_frame')
for scene in scenes:
    medians = {label: statistics.median(r['encode_mean_us'] for r in results if r['scene']==scene and r['version']==label) for label in binaries}
    payload = statistics.median(r['mean_bytes'] for r in results if r['scene']==scene and r['version']=='after')
    print(scene, round(medians['before'], 3), round(medians['after'], 3), round(100*(1-medians['after']/medians['before']), 1), payload)
