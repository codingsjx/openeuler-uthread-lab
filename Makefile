CC ?= cc
CPPFLAGS += -Iinclude
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic

.PHONY: all check benchmark report perf-check clean
all: build/demo build/test_runtime build/test_trace_limit build/benchmark
build:
	mkdir -p build
build/demo: src/uthread.c examples/demo.c include/uthread.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/uthread.c examples/demo.c -o $@
build/test_runtime: src/uthread.c tests/test_runtime.c include/uthread.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/uthread.c tests/test_runtime.c -o $@
build/test_trace_limit: src/uthread.c tests/test_trace_limit.c include/uthread.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -DUT_TRACE_LIMIT=16 src/uthread.c tests/test_trace_limit.c -o $@
build/benchmark: src/uthread.c examples/benchmark.c include/uthread.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread src/uthread.c examples/benchmark.c -o $@
build/benchmark-baseline: benchmarks/baseline/uthread.c examples/benchmark.c include/uthread.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread benchmarks/baseline/uthread.c examples/benchmark.c -o $@
build/scheduler-baseline: benchmarks/baseline/uthread.c examples/scheduler_benchmark.c include/uthread.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) benchmarks/baseline/uthread.c examples/scheduler_benchmark.c -o $@
build/scheduler-optimized: src/uthread.c examples/scheduler_benchmark.c include/uthread.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -DHAVE_TRACE_OPTIONS src/uthread.c examples/scheduler_benchmark.c -o $@
check: all
	python3 scripts/run_experiments.py
benchmark: all
	python3 scripts/benchmark.py
report:
	python3 scripts/make_report.py
perf-check: all build/benchmark-baseline build/scheduler-baseline build/scheduler-optimized
	python3 scripts/measure_optimization.py
clean:
	rm -f build/demo build/test_runtime build/test_trace_limit build/benchmark build/benchmark-baseline build/scheduler-baseline build/scheduler-optimized
