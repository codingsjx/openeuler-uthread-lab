# Runtime optimization measurements

Baseline and optimized binaries were built with the same flags and pinned to the same allowed CPU.
Full tracing is enabled for the primary before/after comparison. Trace-off changes observability and is shown separately.
Scheduler times include initialization, creation and execution, but exclude destruction and trace export.

| Tasks | Variant | Runs | Wall median us | Q1 us | Q3 us |
| --- | --- | --- | --- | --- | --- |
| 1 | baseline | 20 | 1966.500 | 1761.250 | 2240.000 |
| 1 | optimized | 20 | 243.500 | 239.750 | 253.000 |
| 1 | trace_off | 20 | 62.000 | 58.500 | 66.000 |
| 8 | baseline | 20 | 13705.500 | 13065.750 | 14132.000 |
| 8 | optimized | 20 | 11518.500 | 11212.000 | 12098.000 |
| 8 | trace_off | 20 | 9198.500 | 8371.750 | 9598.500 |
| 32 | baseline | 20 | 46099.000 | 44115.250 | 49677.500 |
| 32 | optimized | 20 | 47316.000 | 42704.000 | 47974.000 |
| 32 | trace_off | 20 | 32655.500 | 30473.250 | 34758.500 |

## I/O cases

| Input delay ms | Iterations per step | Variant | Runs | Wall median ms | First work median ms |
| --- | --- | --- | --- | --- | --- |
| 5 | 10000 | baseline | 10 | 8.382 | 0.206 |
| 5 | 10000 | optimized | 10 | 8.832 | 0.195 |
| 5 | 1000000 | baseline | 10 | 106.969 | 0.205 |
| 5 | 1000000 | optimized | 10 | 111.160 | 0.185 |
| 100 | 10000 | baseline | 10 | 104.177 | 0.256 |
| 100 | 10000 | optimized | 10 | 106.341 | 0.348 |
| 100 | 1000000 | baseline | 10 | 108.553 | 0.488 |
| 100 | 1000000 | optimized | 10 | 110.557 | 0.497 |

## Limits

Shared/containerized or virtualized runners are noisy. These results concern the recorded workloads only.
The trace-off variant cannot provide an event timeline. A lower wall median does not establish a universal speedup.
Check optimization.json for all raw samples, source hashes, environment and workload checksums.
