"""Build a standalone report from real CSV traces and a completed benchmark."""
import argparse
import json
from pathlib import Path
from benchmark import read_trace

ROOT = Path(__file__).resolve().parents[1]

def embed_json(value):
    return json.dumps(value, ensure_ascii=False).replace('<', '\\u003c').replace('>', '\\u003e').replace('&', '\\u0026')

def collect(results, benchmark_file=None):
    traces = []
    for name, label in [('schedule-first','首个就绪任务优先'), ('schedule-rr','轮转调度'),
                        ('io-blocking','阻塞 I/O 对照'), ('io-event','事件驱动 I/O')]:
        path = results/(name+'.csv')
        if path.exists():
            traces.append({'name': label, 'file': path.name, 'rows': read_trace(path)})
    data = None
    if benchmark_file:
        data = json.loads(benchmark_file.read_text(encoding='utf-8'))
        if data.get('status') != 'passed':
            raise ValueError('cannot present failed or incomplete measurements as passed')
        base = benchmark_file.parent.resolve()
        for mode in ('blocking', 'event', 'pthread'):
            sample = next(s for s in data['samples'] if s['mode'] == mode)
            path = (base/sample['trace']).resolve()
            if not path.is_relative_to(base):
                raise ValueError('trace path escapes benchmark directory')
            traces.append({'name': '重复实验 '+mode+' 首轮事件', 'file': path.name, 'rows': read_trace(path)})
            runtime = Path(str(path)+'.runtime.csv')
            if runtime.exists():
                traces.append({'name': '重复实验 '+mode+' 运行时状态', 'file': runtime.name, 'rows': read_trace(runtime)})
    if not traces:
        raise ValueError('no real traces found; run make check first')
    return {'traces': traces, 'benchmark': data}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--results', type=Path, default=ROOT/'results')
    parser.add_argument('--benchmark', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.benchmark is None:
        candidates = sorted(args.results.glob('benchmark-*/benchmark.json'))
        for p in reversed(candidates):
            if json.loads(p.read_text(encoding='utf-8')).get('status') == 'passed':
                args.benchmark = p
                break
    payload = collect(args.results, args.benchmark)
    template = (Path(__file__).parent/'report_template.html').read_text(encoding='utf-8')
    output = args.output or args.results/'report.html'
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(template.replace('__REPORT_JSON__', embed_json(payload)), encoding='utf-8')
    print('REPORT='+str(output))

if __name__ == '__main__':
    main()
