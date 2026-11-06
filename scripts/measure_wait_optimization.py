"""Compare round 2 against the frozen first optimization, with full tracing."""
import argparse
import datetime
import hashlib
import json
import os
import platform
import random
import subprocess
import uuid
from pathlib import Path
from benchmark import distribution

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repeats', type=int, default=20)
    args = parser.parse_args()
    if platform.system() != 'Linux' or args.repeats < 3:
        parser.error('Linux and at least 3 repeats are required')
    cpu = min(os.sched_getaffinity(0))
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    out = ROOT/'results'/('wait-optimization-'+stamp+'-'+uuid.uuid4().hex[:6])
    out.mkdir(parents=True)
    files = ['src/uthread.c', 'include/uthread.h', 'benchmarks/round1/uthread.c',
             'examples/wait_benchmark.c', 'examples/scheduler_benchmark.c',
             'scripts/measure_wait_optimization.py', 'Makefile']
    data = {'status': 'running', 'baseline_revision':
            (ROOT/'benchmarks/round1/REVISION').read_text().strip(),
            'environment': {'os_release': platform.freedesktop_os_release(),
                            'uname': platform.uname()._asdict(), 'cpu_affinity': [cpu],
                            'github_actions': os.environ.get('GITHUB_ACTIONS', 'false'),
                            'compiler': subprocess.check_output(['cc', '--version'], text=True).splitlines()[0]},
            'source_sha256': {p: hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in files},
            'parameters': vars(args), 'samples': [], 'summary': []}
    rng = random.Random(20261026)
    cases = [('scheduler', str(n), [n, 1000, 'on']) for n in (1, 8, 32)]
    cases += [('wait', f'{sharing}-{n}', [n, sharing, 1000, 'on'])
              for sharing in ('shared', 'distinct') for n in (1, 8, 31)]
    try:
        for kind, case, arguments in cases:
            expected = None
            for round_id in range(-1, args.repeats):
                order = ['round1', 'optimized']
                rng.shuffle(order)
                for variant in order:
                    binary = f'{kind}-{variant}'
                    proc = subprocess.run([str(ROOT/'build'/binary), *map(str, arguments)],
                                          capture_output=True, text=True, check=True, timeout=30,
                                          cwd=ROOT, preexec_fn=lambda: os.sched_setaffinity(0, {cpu}))
                    sample = json.loads(proc.stdout)
                    count = arguments[0]
                    if sample['completed'] != count+(kind == 'wait') or sample['yields'] != 1000:
                        raise ValueError('Incomplete workload: '+case)
                    if sample['trace'] != 'on':
                        raise ValueError('Tracing differs: '+case)
                    identity = (sample['completed'], sample['checksum'])
                    if expected is None:
                        expected = identity
                    if identity != expected:
                        raise ValueError('Workload checksum differs: '+case)
                    if round_id >= 0:
                        data['samples'].append(dict(sample, kind=kind, case=case,
                                                    variant=variant, round=round_id))
            for variant in ('round1', 'optimized'):
                samples = [s for s in data['samples'] if s['kind'] == kind and
                           s['case'] == case and s['variant'] == variant]
                metrics = ['wall_us', 'cpu_us']
                if kind == 'wait':
                    metrics += ['poll_calls', 'dispatches']
                data['summary'].append({'kind': kind, 'case': case, 'variant': variant,
                                        **{m: distribution([s[m] for s in samples]) for m in metrics}})
            print('Completed:', kind, case, flush=True)
        data['status'] = 'passed'
    except Exception as error:
        data.update(status='failed', error=str(error))
        raise
    finally:
        (out/'measurements.json').write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding='utf-8')
    lines = ['# Round 2 measurements', '',
             'Frozen round 1 and current runtime, identical compiler flags, full tracing on both.',
             'One warm-up per variant/case; randomized order per repetition; same allowed CPU.',
             'Times include initialization, task creation and execution; exclude trace export and destruction.', '',
             '| Workload | Case | Variant | Runs | Wall median us | Q1 | Q3 |',
             '| --- | --- | --- | --- | --- | --- | --- |']
    for row in data['summary']:
        s = row['wall_us']
        lines.append(f"| {row['kind']} | {row['case']} | {row['variant']} | {s['n']} | {s['median']:.1f} | {s['q1']:.1f} | {s['q3']:.1f} |")
    lines += ['', 'Shared means all readers wait on one pipe; distinct means one pipe per reader.',
              'A producer yields 1000 times before writing one byte per reader. Setup of pipes is excluded.',
              'These synthetic workloads measure registry/scheduling overhead, not real service throughput.',
              'Container runners can be noisy. See measurements.json for raw samples, CPU times, poll counts, hashes and environment.']
    (out/'report.md').write_text('\n'.join(lines)+'\n', encoding='utf-8')
    print('WAIT_OPTIMIZATION_RESULT='+str(out), flush=True)


if __name__ == '__main__':
    main()
