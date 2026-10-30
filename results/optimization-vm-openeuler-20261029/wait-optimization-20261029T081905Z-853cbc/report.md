# Round 2 measurements

Frozen round 1 and current runtime, identical compiler flags, full tracing on both.
One warm-up per variant/case; randomized order per repetition; same allowed CPU.
Times include initialization, task creation and execution; exclude trace export and destruction.

| Workload | Case | Variant | Runs | Wall median us | Q1 | Q3 |
| --- | --- | --- | --- | --- | --- | --- |
| scheduler | 1 | round1 | 20 | 269.0 | 253.8 | 575.8 |
| scheduler | 1 | optimized | 20 | 311.0 | 255.0 | 564.0 |
| scheduler | 8 | round1 | 20 | 11777.5 | 11190.5 | 12635.8 |
| scheduler | 8 | optimized | 20 | 11965.0 | 11306.2 | 12262.5 |
| scheduler | 32 | round1 | 20 | 45410.0 | 43549.5 | 46985.5 |
| scheduler | 32 | optimized | 20 | 46386.5 | 44453.8 | 47469.5 |
| wait | shared-1 | round1 | 20 | 2188.0 | 1893.0 | 2368.2 |
| wait | shared-1 | optimized | 20 | 2256.0 | 2115.5 | 2434.8 |
| wait | shared-8 | round1 | 20 | 2936.0 | 2701.5 | 3210.8 |
| wait | shared-8 | optimized | 20 | 2478.5 | 2111.2 | 2792.8 |
| wait | shared-31 | round1 | 20 | 5409.5 | 4808.5 | 5718.0 |
| wait | shared-31 | optimized | 20 | 3118.5 | 2977.2 | 3638.0 |
| wait | distinct-1 | round1 | 20 | 2077.5 | 1877.8 | 2208.0 |
| wait | distinct-1 | optimized | 20 | 2136.5 | 1861.5 | 2650.8 |
| wait | distinct-8 | round1 | 20 | 3141.0 | 2873.5 | 3356.5 |
| wait | distinct-8 | optimized | 20 | 2822.5 | 2563.2 | 2981.0 |
| wait | distinct-31 | round1 | 20 | 5450.5 | 5097.0 | 5928.5 |
| wait | distinct-31 | optimized | 20 | 4886.0 | 4607.2 | 5104.5 |

Shared means all readers wait on one pipe; distinct means one pipe per reader.
A producer yields 1000 times before writing one byte per reader. Setup of pipes is excluded.
These synthetic workloads measure registry/scheduling overhead, not real service throughput.
Container runners can be noisy. See measurements.json for raw samples, CPU times, poll counts, hashes and environment.
