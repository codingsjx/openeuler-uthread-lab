# Round 2 measurements

Frozen round 1 and current runtime, identical compiler flags, full tracing on both.
One warm-up per variant/case; randomized order per repetition; same allowed CPU.
Times include initialization, task creation and execution; exclude trace export and destruction.

| Workload | Case | Variant | Runs | Wall median us | Q1 | Q3 |
| --- | --- | --- | --- | --- | --- | --- |
| scheduler | 1 | round1 | 20 | 125.0 | 123.0 | 127.0 |
| scheduler | 1 | optimized | 20 | 123.0 | 122.8 | 126.8 |
| scheduler | 8 | round1 | 20 | 5297.0 | 5269.5 | 5322.5 |
| scheduler | 8 | optimized | 20 | 5272.5 | 5255.5 | 5299.8 |
| scheduler | 32 | round1 | 20 | 21293.5 | 21268.5 | 21350.2 |
| scheduler | 32 | optimized | 20 | 21218.5 | 21176.8 | 21266.8 |
| wait | shared-1 | round1 | 20 | 977.5 | 975.0 | 992.8 |
| wait | shared-1 | optimized | 20 | 968.0 | 962.8 | 986.8 |
| wait | shared-8 | round1 | 20 | 1388.5 | 1358.2 | 1405.5 |
| wait | shared-8 | optimized | 20 | 1126.5 | 1116.2 | 1166.5 |
| wait | shared-31 | round1 | 20 | 2589.5 | 2570.2 | 2608.2 |
| wait | shared-31 | optimized | 20 | 1448.5 | 1430.5 | 1470.0 |
| wait | distinct-1 | round1 | 20 | 998.5 | 991.5 | 1010.5 |
| wait | distinct-1 | optimized | 20 | 987.5 | 978.0 | 1013.5 |
| wait | distinct-8 | round1 | 20 | 1387.5 | 1369.0 | 1396.8 |
| wait | distinct-8 | optimized | 20 | 1330.5 | 1322.5 | 1361.0 |
| wait | distinct-31 | round1 | 20 | 2670.0 | 2636.8 | 2700.5 |
| wait | distinct-31 | optimized | 20 | 2458.0 | 2411.5 | 2490.5 |

Shared means all readers wait on one pipe; distinct means one pipe per reader.
A producer yields 1000 times before writing one byte per reader. Setup of pipes is excluded.
These synthetic workloads measure registry/scheduling overhead, not real service throughput.
Container runners can be noisy. See measurements.json for raw samples, CPU times, poll counts, hashes and environment.
