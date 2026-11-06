"""Bounded socket request/reply and runtime-lifecycle evidence, on Linux only."""
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import signal
import subprocess
import uuid

ROOT = Path(__file__).resolve().parents[1]


def run(command, timeout=90):
    process = subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, start_new_session=True)
    try:
        output, _ = process.communicate(timeout=timeout)
    except BaseException:
        os.killpg(process.pid, signal.SIGKILL)
        process.communicate()
        raise
    return process.returncode, output


def main():
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    destination = ROOT / 'results' / ('service-' + stamp + '-' + uuid.uuid4().hex[:6])
    destination.mkdir(parents=True)
    files = ['src/uthread.c', 'include/uthread.h', 'examples/socket_service.c',
             'tests/test_lifecycle.c', 'scripts/validate_service.py', 'Makefile']
    result = {'status': 'running', 'environment': platform.uname()._asdict(),
              'os_release': platform.freedesktop_os_release(),
              'source_sha256': {name: hashlib.sha256((ROOT/name).read_bytes()).hexdigest()
                                for name in files}, 'runs': []}
    try:
        # No narrow latency or throughput thresholds: preserve the observations.
        for repetition in range(3):
            for mode in (('blocking', 'event') if repetition % 2 == 0 else ('event', 'blocking')):
                trace = destination / f'{mode}-{repetition}.csv'
                command = ['./build/socket_service', mode, str(trace)]
                code, output = run(command, 20)
                (destination / f'{mode}-{repetition}.log').write_text(output, encoding='utf-8')
                item = {'argv': command, 'exit_code': code, 'sample': json.loads(output)}
                result['runs'].append(item)
                if code or item['sample']['status'] != 'passed':
                    raise RuntimeError('Request/reply validation failed')
        code, output = run(['./build/test_lifecycle'])
        (destination / 'lifecycle.log').write_text(output, encoding='utf-8')
        result['lifecycle'] = {'exit_code': code, 'sample': json.loads(output)}
        if code or result['lifecycle']['sample']['status'] != 'passed':
            raise RuntimeError('Lifecycle validation failed')
        result['status'] = 'passed'
    except Exception as error:
        result.update(status='failed', error=str(error))
    (destination / 'service.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps({'status': result['status'], 'results': str(destination)}))
    return 0 if result['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
