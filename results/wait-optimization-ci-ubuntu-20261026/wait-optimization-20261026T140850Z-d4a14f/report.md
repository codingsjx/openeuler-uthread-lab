# Round 2 measurements

Frozen round 1 and current runtime, identical compiler flags, full tracing on both.
One warm-up per variant/case; randomized order per repetition; same allowed CPU.
Times include initialization, task creation and execution; exclude trace export and destruction.

| Workload | Case | Variant | Runs | Wall median us | Q1 | Q3 |
| --- | --- | --- | --- | --- | --- | --- |
| scheduler | 1 | round1 | 20 | 130.0 | 130.0 | 131.2 |
| scheduler | 1 | optimized | 20 | 130.0 | 129.8 | 131.0 |
| scheduler | 8 | round1 | 20 | 8491.0 | 8482.0 | 8516.0 |
| scheduler | 8 | optimized | 20 | 8493.0 | 8458.2 | 8534.5 |
| scheduler | 32 | round1 | 20 | 34212.5 | 33975.2 | 34476.2 |
| scheduler | 32 | optimized | 20 | 34119.5 | 34024.0 | 34463.0 |
| wait | shared-1 | round1 | 20 | 1617.5 | 1606.5 | 1634.8 |
| wait | shared-1 | optimized | 20 | 1621.0 | 1609.5 | 1646.0 |
| wait | shared-8 | round1 | 20 | 1992.5 | 1983.8 | 2012.5 |
| wait | shared-8 | optimized | 20 | 1787.5 | 1777.0 | 1802.5 |
| wait | shared-31 | round1 | 20 | 3241.0 | 3222.2 | 3249.0 |
| wait | shared-31 | optimized | 20 | 2158.5 | 2147.5 | 2205.2 |
| wait | distinct-1 | round1 | 20 | 1639.5 | 1624.5 | 1675.0 |
| wait | distinct-1 | optimized | 20 | 1618.0 | 1610.5 | 1634.5 |
| wait | distinct-8 | round1 | 20 | 2028.5 | 2010.5 | 2140.0 |
| wait | distinct-8 | optimized | 20 | 1976.5 | 1955.8 | 2175.0 |
| wait | distinct-31 | round1 | 20 | 3290.0 | 3267.0 | 3318.0 |
| wait | distinct-31 | optimized | 20 | 3114.0 | 3094.5 | 3156.5 |

Shared means all readers wait on one pipe; distinct means one pipe per reader.
A producer yields 1000 times before writing one byte per reader. Setup of pipes is excluded.
These synthetic workloads measure registry/scheduling overhead, not real service throughput.
Container runners can be noisy. See measurements.json for raw samples, CPU times, poll counts, hashes and environment.
