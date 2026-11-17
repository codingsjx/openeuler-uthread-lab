CC ?= cc
NODE ?= node
CPPFLAGS += -Iinclude
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic

.PHONY: all check check-linux check-functional viewer-check request-check benchmark report perf-check wait-perf-check service-check tcp-check clean
all: build/demo build/test_runtime build/test_trace_limit build/test_wait_registry build/benchmark
build:
	mkdir -p build
build/demo: src/uthread.c examples/demo.c include/uthread.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/uthread.c examples/demo.c -o $@
build/test_runtime: src/uthread.c tests/test_runtime.c include/uthread.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/uthread.c tests/test_runtime.c -o $@
build/test_trace_limit: src/uthread.c tests/test_trace_limit.c include/uthread.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -DUT_TRACE_LIMIT=16 src/uthread.c tests/test_trace_limit.c -o $@
build/test_wait_registry: src/uthread.c tests/test_wait_registry.c include/uthread.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/uthread.c tests/test_wait_registry.c -Wl,--wrap=poll,--wrap=__poll_chk -o $@
build/test_write: src/uthread.c tests/test_write.c include/uthread.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/uthread.c tests/test_write.c -Wl,--wrap=poll,--wrap=__poll_chk -o $@
build/tcp_echo: src/uthread.c examples/tcp_echo.c include/uthread.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/uthread.c examples/tcp_echo.c -o $@
build/request_server: src/uthread.c examples/request_server.c examples/request_policy.c include/uthread.h include/request_policy.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/uthread.c examples/request_server.c examples/request_policy.c -o $@
build/test_request_policy: examples/request_policy.c tests/test_request_policy.c include/request_policy.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) examples/request_policy.c tests/test_request_policy.c -o $@
request-check: build/request_server build/test_request_policy
	./build/test_request_policy
	python3 -m unittest discover -s tests -p test_request_server.py -v
check-linux: check tcp-check request-check
	python3 -m unittest discover -s tests -p test_reporting.py -v
viewer-check:
	$(NODE) --test tests/test_trace_core.cjs
check-functional: check-linux viewer-check
build/test_dynamic: src/uthread.c tests/test_dynamic.c include/uthread.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/uthread.c tests/test_dynamic.c -o $@
build/test_cancel: src/uthread.c tests/test_cancel.c include/uthread.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/uthread.c tests/test_cancel.c -o $@
tcp-check: build/test_write build/test_dynamic build/test_cancel build/tcp_echo
	./build/test_write
	./build/test_dynamic
	./build/test_cancel
	python3 -m unittest discover -s tests -p test_tcp.py -v
build/benchmark: src/uthread.c examples/benchmark.c include/uthread.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread src/uthread.c examples/benchmark.c -o $@
build/benchmark-baseline: benchmarks/baseline/uthread.c examples/benchmark.c include/uthread.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread benchmarks/baseline/uthread.c examples/benchmark.c -o $@
build/scheduler-baseline: benchmarks/baseline/uthread.c examples/scheduler_benchmark.c include/uthread.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) benchmarks/baseline/uthread.c examples/scheduler_benchmark.c -o $@
build/scheduler-optimized: src/uthread.c examples/scheduler_benchmark.c include/uthread.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -DHAVE_TRACE_OPTIONS src/uthread.c examples/scheduler_benchmark.c -o $@
build/scheduler-round1: benchmarks/round1/uthread.c examples/scheduler_benchmark.c include/uthread.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -DHAVE_TRACE_OPTIONS benchmarks/round1/uthread.c examples/scheduler_benchmark.c -o $@
build/wait-round1: benchmarks/round1/uthread.c examples/wait_benchmark.c include/uthread.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) benchmarks/round1/uthread.c examples/wait_benchmark.c -o $@
build/wait-optimized: src/uthread.c examples/wait_benchmark.c include/uthread.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/uthread.c examples/wait_benchmark.c -o $@
check: all
	python3 scripts/run_experiments.py
benchmark: all
	python3 scripts/benchmark.py
report:
	python3 scripts/make_report.py
build/socket_service: src/uthread.c examples/socket_service.c include/uthread.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/uthread.c examples/socket_service.c -o $@
build/test_lifecycle: src/uthread.c tests/test_lifecycle.c include/uthread.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/uthread.c tests/test_lifecycle.c -o $@
service-check: build/socket_service build/test_lifecycle
	python3 scripts/validate_service.py
perf-check: all build/benchmark-baseline build/scheduler-baseline build/scheduler-optimized
	python3 scripts/measure_optimization.py
wait-perf-check: all build/scheduler-round1 build/scheduler-optimized build/wait-round1 build/wait-optimized
	python3 scripts/measure_wait_optimization.py
clean:
	rm -f build/request_server build/test_request_policy
	rm -f build/test_cancel
	rm -f build/test_dynamic
	rm -f build/tcp_echo build/test_write
	rm -f build/socket_service build/test_lifecycle
	rm -f build/demo build/test_runtime build/test_trace_limit build/test_wait_registry build/benchmark build/benchmark-baseline build/scheduler-baseline build/scheduler-optimized build/scheduler-round1 build/wait-round1 build/wait-optimized
