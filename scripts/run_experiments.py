"""Run actual Linux binaries and validate the traces they produce. No mock metrics."""
import csv
import json
import platform
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / 'results'

def run(*args):
    subprocess.run(args, cwd=ROOT, check=True, timeout=10)

def trace(name, *args):
    path = RESULTS / (name + '.csv')
    run(str(ROOT / 'build/demo'), *args, str(path))
    with path.open(newline='', encoding='utf-8') as f:
        rows = list(csv.DictReader(f))
    assert rows and not any(r['event'] == 'dropped' for r in rows), 'trace incomplete'
    times = [int(r['time_us']) for r in rows]
    assert times == sorted(times), 'timestamps not monotonic'
    return rows

def main():
    if platform.system() != 'Linux':
        raise SystemExit('Linux required. Run make check inside openEuler; Windows MinGW is not supported.')
    RESULTS.mkdir(exist_ok=True)
    run(str(ROOT / 'build/test_runtime'))
    first = trace('schedule-first', 'schedule', 'first')
    rr = trace('schedule-rr', 'schedule', 'rr')
    marks = lambda rows: [r['detail'] for r in rows if r['event'] == 'mark']
    assert marks(first) == list('AAAABBBBCCCC'), 'unexpected first-ready ordering'
    assert marks(rr) == list('ABCABCABCABC'), 'round-robin ordering failed'
    summary = {'status': 'passed', 'environment': platform.uname()._asdict(), 'io': {}}
    for mode in ('blocking', 'event'):
        rows = trace('io-' + mode, 'io', mode)
        finish = next(int(r['time_us']) for r in rows if r['detail'] == 'read_end')
        ticks = [int(r['time_us']) for r in rows if r['detail'] == 'worker_tick']
        assert len(ticks) == 40, 'lost worker steps'
        before = sum(t < finish for t in ticks)
        assert (before == 0 if mode == 'blocking' else before > 0), 'I/O isolation check failed'
        summary['io'][mode] = {'worker_ticks_before_read_end': before,
                               'first_worker_tick_us': ticks[0], 'read_end_us': finish}
    summary['note'] = 'Single smoke experiment; not a benchmark or statistical performance claim.'
    (RESULTS / 'summary.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(summary, ensure_ascii=False, indent=2))

if __name__ == '__main__':
    main()
