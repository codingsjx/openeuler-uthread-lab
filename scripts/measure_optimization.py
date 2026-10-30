"""Compare the frozen runtime and optimized runtime in the same environment."""
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
from benchmark import distribution, read_trace

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repeats',type=int,default=20)
    parser.add_argument('--io-repeats',type=int,default=10)
    args=parser.parse_args()
    if platform.system()!='Linux' or args.repeats<3 or args.io_repeats<3:
        parser.error('Linux and at least 3 repeats are required')
    cpu=min(os.sched_getaffinity(0))
    stamp=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    out=ROOT/'results'/('optimization-'+stamp+'-'+uuid.uuid4().hex[:6])
    out.mkdir(parents=True)
    (out/'traces').mkdir()
    files=['src/uthread.c','include/uthread.h','benchmarks/baseline/uthread.c',
           'examples/scheduler_benchmark.c','examples/benchmark.c','Makefile','scripts/measure_optimization.py']
    data={'status':'running','baseline_revision':(ROOT/'benchmarks/baseline/REVISION').read_text().strip(),
          'environment':{'os_release':platform.freedesktop_os_release(),'uname':platform.uname()._asdict(),
                         'cpu_affinity':[cpu],'github_actions':os.environ.get('GITHUB_ACTIONS','false'),
                         'compiler':subprocess.check_output(['cc','--version'],text=True).splitlines()[0]},
          'source_sha256':{p:hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in files},
          'parameters':vars(args),'scheduler':[],'io':[]}
    result=out/'optimization.json'
    rng=random.Random(20261026)
    def execute(binary,arguments):
        completed=subprocess.run([str(ROOT/'build'/binary),*map(str,arguments)],capture_output=True,
                                 text=True,check=True,timeout=30,cwd=ROOT,
                                 preexec_fn=lambda:os.sched_setaffinity(0,{cpu}))
        return json.loads(completed.stdout)
    try:
        for tasks in (1,8,32):
            expected=None
            for round_id in range(-1,args.repeats):
                order=['baseline','optimized','trace_off'];rng.shuffle(order)
                for variant in order:
                    trace='off' if variant=='trace_off' else 'on'
                    binary='scheduler-baseline' if variant=='baseline' else 'scheduler-optimized'
                    sample=execute(binary,[tasks,1000,trace])
                    if sample['completed']!=tasks or sample['tasks']!=tasks or sample['yields']!=1000:
                        raise ValueError('scheduler work differs')
                    if expected is None:expected=sample['checksum']
                    if sample['checksum']!=expected:raise ValueError('scheduler checksum differs')
                    if round_id>=0:data['scheduler'].append(dict(sample,variant=variant,round=round_id))
            print('Scheduler task count complete:',tasks,flush=True)
        for delay in (5,100):
            for iterations in (10000,1000000):
                expected=None
                for round_id in range(-1,args.io_repeats):
                    order=['baseline','optimized'];rng.shuffle(order)
                    for variant in order:
                        trace=out/'traces'/f'io-{delay}-{iterations}-{round_id}-{variant}.csv'
                        binary='benchmark-baseline' if variant=='baseline' else 'benchmark'
                        sample=execute(binary,['event',40,iterations,delay,trace])
                        rows=read_trace(trace)
                        ticks=[r for r in rows if r['detail']=='worker_tick']
                        if len(ticks)!=40 or sample['steps']!=40:raise ValueError('I/O workload incomplete')
                        if expected is None:expected=sample['checksum']
                        if sample['checksum']!=expected:raise ValueError('I/O checksum differs')
                        if round_id>=0:
                            data['io'].append(dict(sample,variant=variant,round=round_id,
                                                   trace=str(trace.relative_to(out))))
                print('I/O case complete:',delay,iterations,flush=True)
        data['status']='passed'
        data['scheduler_summary']=[]
        for tasks in (1,8,32):
            for variant in ('baseline','optimized','trace_off'):
                samples=[s for s in data['scheduler'] if s['tasks']==tasks and s['variant']==variant]
                data['scheduler_summary'].append({'tasks':tasks,'variant':variant,
                    'wall_us':distribution([s['wall_us'] for s in samples]),
                    'cpu_us':distribution([s['cpu_us'] for s in samples])})
        data['io_summary']=[]
        for delay in (5,100):
            for iterations in (10000,1000000):
                for variant in ('baseline','optimized'):
                    samples=[s for s in data['io'] if s['delay_ms']==delay and s['iterations']==iterations and s['variant']==variant]
                    data['io_summary'].append({'delay_ms':delay,'iterations':iterations,'variant':variant,
                        **{metric:distribution([s[metric] for s in samples])
                           for metric in ('wall_us','cpu_us','first_worker_tick_us')}})
    except Exception as error:
        data.update(status='failed',error=str(error));raise
    finally:
        result.write_text(json.dumps(data,ensure_ascii=False,indent=2),encoding='utf-8')
    lines=['# Runtime optimization measurements','',
           'Baseline and optimized binaries were built with the same flags and pinned to the same allowed CPU.',
           'Full tracing is enabled for the primary before/after comparison. Trace-off changes observability and is shown separately.',
           'Scheduler times include initialization, creation and execution, but exclude destruction and trace export.','',
           '| Tasks | Variant | Runs | Wall median us | Q1 us | Q3 us |',
           '| --- | --- | --- | --- | --- | --- |']
    for row in data['scheduler_summary']:
        s=row['wall_us'];lines.append(f"| {row['tasks']} | {row['variant']} | {s['n']} | {s['median']:.3f} | {s['q1']:.3f} | {s['q3']:.3f} |")
    lines+=['','## I/O cases','', '| Input delay ms | Iterations per step | Variant | Runs | Wall median ms | First work median ms |',
            '| --- | --- | --- | --- | --- | --- |']
    for row in data['io_summary']:
        lines.append(f"| {row['delay_ms']} | {row['iterations']} | {row['variant']} | {row['wall_us']['n']} | {row['wall_us']['median']/1000:.3f} | {row['first_worker_tick_us']['median']/1000:.3f} |")
    lines+=['','## Limits','',
            'Shared/containerized or virtualized runners are noisy. These results concern the recorded workloads only.',
            'The trace-off variant cannot provide an event timeline. A lower wall median does not establish a universal speedup.',
            'Check optimization.json for all raw samples, source hashes, environment and workload checksums.']
    (out/'report.md').write_text('\n'.join(lines)+'\n',encoding='utf-8')
    print('OPTIMIZATION_RESULT='+str(result),flush=True)

if __name__=='__main__':main()
