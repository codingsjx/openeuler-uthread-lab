"""Balanced repeated measurements with one CPU and identical fixed work."""
import argparse
import csv
import datetime
import hashlib
import json
import os
import platform
import random
import statistics
import subprocess
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODES = ('blocking', 'event', 'pthread')
METRICS = ('wall_us', 'cpu_us', 'max_rss_kb', 'first_worker_tick_us',
           'ticks_before_read_end')

def distribution(values):
    q1, _, q3 = statistics.quantiles(values, n=4, method='inclusive')
    return {'n': len(values), 'median': statistics.median(values),
            'q1': q1, 'q3': q3, 'minimum': min(values), 'maximum': max(values)}

def summarize(samples):
    return {mode: {metric: distribution([s[metric] for s in samples if s['mode'] == mode])
                   for metric in METRICS} for mode in MODES}

def version(*args):
    return subprocess.check_output(args, text=True).splitlines()[0]

def read_trace(path):
    with Path(path).open(newline='', encoding='utf-8') as source:
        reader = csv.DictReader(source)
        if reader.fieldnames != ['time_us', 'tid', 'event', 'detail']:
            raise ValueError('invalid trace columns')
        rows = list(reader)
    previous = -1
    for row in rows:
        timestamp = int(row['time_us'])
        row['tid'] = int(row['tid'])
        row['time_us'] = timestamp
        if timestamp < previous or row['event'] == 'dropped':
            raise ValueError('incomplete or non-monotonic trace')
        previous = timestamp
    if not rows:
        raise ValueError('empty trace')
    return rows

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repeats', type=int, default=20)
    parser.add_argument('--warmup', type=int, default=1)
    parser.add_argument('--steps', type=int, default=40)
    parser.add_argument('--iterations', type=int, default=100000)
    parser.add_argument('--delay-ms', type=int, default=100)
    parser.add_argument('--seed', type=int, default=20261025)
    parser.add_argument('--cpu', type=int)
    args = parser.parse_args()
    if platform.system() != 'Linux':
        parser.error('run on Linux/openEuler')
    if args.repeats < 3 or args.warmup < 0 or args.warmup > 20:
        parser.error('at least 3 measured repetitions; warmup in 0..20')
    if not (1 <= args.steps <= 10000 and 1 <= args.iterations <= 10000000 and 1 <= args.delay_ms <= 5000):
        parser.error('invalid workload parameters')
    available = sorted(os.sched_getaffinity(0))
    cpu = available[0] if args.cpu is None else args.cpu
    if cpu not in available:
        parser.error('CPU is outside current affinity mask')
    session = ROOT / 'results' / ('benchmark-' + datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '-' + uuid.uuid4().hex[:6])
    session.mkdir(parents=True)
    (session / 'traces').mkdir()
    sources = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
               for p in [ROOT/'src/uthread.c', ROOT/'examples/benchmark.c', ROOT/'Makefile']}
    environment = {'uname': platform.uname()._asdict(), 'os_release': platform.freedesktop_os_release(),
                   'compiler': version('cc', '--version'), 'libc': platform.libc_ver(),
                   'python': platform.python_version(), 'available_cpus': available,
                   'child_cpu_affinity': [cpu], 'sources_sha256': sources}
    report = {'status': 'running', 'environment': environment, 'parameters': vars(args), 'samples': []}
    output = session / 'benchmark.json'
    output.write_text(json.dumps(report, indent=2), encoding='utf-8')
    rng = random.Random(args.seed)
    expected_checksum = None
    try:
        for round_id in range(-args.warmup, args.repeats):
            order = list(MODES)
            rng.shuffle(order)
            for position, mode in enumerate(order):
                name = f'{"warmup" if round_id < 0 else "run"}-{abs(round_id):03d}-{mode}.csv'
                path = session/'traces'/name
                cmd = [str(ROOT/'build/benchmark'), mode, str(args.steps), str(args.iterations), str(args.delay_ms), str(path)]
                completed = subprocess.run(cmd, cwd=ROOT, check=True, text=True, capture_output=True,
                                           timeout=30, preexec_fn=lambda: os.sched_setaffinity(0, {cpu}))
                sample = json.loads(completed.stdout)
                if sample['mode'] != mode or sample['steps'] != args.steps or sample['iterations'] != args.iterations or sample['delay_ms'] != args.delay_ms:
                    raise ValueError('binary workload arguments do not match request')
                checksum = sample['checksum']
                if expected_checksum is None:
                    expected_checksum = checksum
                if checksum != expected_checksum:
                    raise ValueError('fixed-work checksum mismatch')
                rows = read_trace(path)
                ticks = [r for r in rows if r['detail'] == 'worker_tick']
                read_end = [r for r in rows if r['detail'] == 'read_end']
                if len(ticks) != args.steps or len(read_end) != 1:
                    raise ValueError('missing worker or reader events')
                before = sum(r['time_us'] < read_end[0]['time_us'] for r in ticks)
                if before != sample['ticks_before_read_end']:
                    raise ValueError('summary disagrees with trace')
                if round_id >= 0:
                    sample.update(round=round_id, position=position, trace='traces/'+name)
                    report['samples'].append(sample)
            display_round = round_id + args.warmup + 1 if round_id < 0 else round_id + 1
            print(f'Completed {"warmup" if round_id < 0 else "measurement"} round {display_round}', flush=True)
        report['summary'] = summarize(report['samples'])
        report['status'] = 'passed'
        report['limitations'] = [
            'Single vCPU affinity inside a VM does not pin a physical host CPU.',
            'All modes perform identical fixed iterations and share a mutex-protected marker logger.',
            'User-thread internal state recording remains enabled; this is an instrumented comparison.',
            'Wall time includes initialization, fork and writer join, but excludes trace file export.',
            'CPU usage covers the parent process and its threads, excluding the separate writer process.',
            'One fixed workload; quartiles describe this run and do not prove a universal performance advantage.'
        ]
    except Exception as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print('BENCHMARK_RESULT='+str(output), flush=True)

if __name__ == '__main__':
    main()
