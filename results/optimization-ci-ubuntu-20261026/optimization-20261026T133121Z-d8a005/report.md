# Runtime optimization measurements

Baseline and optimized binaries were built with the same flags and pinned to the same allowed CPU.
Full tracing is enabled for the primary before/after comparison. Trace-off changes observability and is shown separately.
Scheduler times include initialization, creation and execution, but exclude destruction and trace export.

| Tasks | Variant | Runs | Wall median us | Q1 us | Q3 us |
| --- | --- | --- | --- | --- | --- |
| 1 | baseline | 20 | 691.000 | 672.500 | 704.000 |
| 1 | optimized | 20 | 113.000 | 112.500 | 115.250 |
| 1 | trace_off | 20 | 26.000 | 26.000 | 27.000 |
| 8 | baseline | 20 | 5139.500 | 5105.250 | 5184.000 |
| 8 | optimized | 20 | 4659.000 | 4629.750 | 4692.000 |
| 8 | trace_off | 20 | 3893.000 | 3843.750 | 3924.000 |
| 32 | baseline | 20 | 20839.000 | 20569.250 | 21513.500 |
| 32 | optimized | 20 | 18287.000 | 17996.250 | 18540.000 |
| 32 | trace_off | 20 | 15349.500 | 15149.000 | 15476.500 |

## I/O cases

| Input delay ms | Iterations per step | Variant | Runs | Wall median ms | First work median ms |
| --- | --- | --- | --- | --- | --- |
| 5 | 10000 | baseline | 10 | 5.191 | 0.114 |
| 5 | 10000 | optimized | 10 | 5.194 | 0.108 |
| 5 | 1000000 | baseline | 10 | 57.187 | 0.126 |
| 5 | 1000000 | optimized | 10 | 57.030 | 0.120 |
| 100 | 10000 | baseline | 10 | 100.346 | 0.120 |
| 100 | 10000 | optimized | 10 | 100.359 | 0.116 |
| 100 | 1000000 | baseline | 10 | 100.254 | 0.121 |
| 100 | 1000000 | optimized | 10 | 100.257 | 0.114 |

## Limits

Shared/containerized or virtualized runners are noisy. These results concern the recorded workloads only.
The trace-off variant cannot provide an event timeline. A lower wall median does not establish a universal speedup.
Check optimization.json for all raw samples, source hashes, environment and workload checksums.
