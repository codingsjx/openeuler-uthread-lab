# Runtime optimization measurements

Baseline and optimized binaries were built with the same flags and pinned to the same allowed CPU.
Full tracing is enabled for the primary before/after comparison. Trace-off changes observability and is shown separately.
Scheduler times include initialization, creation and execution, but exclude destruction and trace export.

| Tasks | Variant | Runs | Wall median us | Q1 us | Q3 us |
| --- | --- | --- | --- | --- | --- |
| 1 | baseline | 20 | 1146.500 | 1124.750 | 1177.500 |
| 1 | optimized | 20 | 160.500 | 155.750 | 167.250 |
| 1 | trace_off | 20 | 38.000 | 35.000 | 40.250 |
| 8 | baseline | 20 | 8970.500 | 8796.750 | 9000.250 |
| 8 | optimized | 20 | 7840.500 | 7827.500 | 7884.750 |
| 8 | trace_off | 20 | 6718.500 | 6697.000 | 6724.750 |
| 32 | baseline | 20 | 35490.500 | 35367.750 | 35650.500 |
| 32 | optimized | 20 | 31317.500 | 31227.500 | 31419.750 |
| 32 | trace_off | 20 | 26889.500 | 26812.000 | 26926.500 |

## I/O cases

| Input delay ms | Iterations per step | Variant | Runs | Wall median ms | First work median ms |
| --- | --- | --- | --- | --- | --- |
| 5 | 10000 | baseline | 10 | 5.293 | 0.180 |
| 5 | 10000 | optimized | 10 | 5.303 | 0.168 |
| 5 | 1000000 | baseline | 10 | 75.309 | 0.174 |
| 5 | 1000000 | optimized | 10 | 75.277 | 0.164 |
| 100 | 10000 | baseline | 10 | 100.404 | 0.183 |
| 100 | 10000 | optimized | 10 | 100.417 | 0.184 |
| 100 | 1000000 | baseline | 10 | 100.385 | 0.177 |
| 100 | 1000000 | optimized | 10 | 100.380 | 0.171 |

## Limits

Shared/containerized or virtualized runners are noisy. These results concern the recorded workloads only.
The trace-off variant cannot provide an event timeline. A lower wall median does not establish a universal speedup.
Check optimization.json for all raw samples, source hashes, environment and workload checksums.
