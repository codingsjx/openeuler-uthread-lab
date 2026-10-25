CC ?= cc
CPPFLAGS += -Iinclude
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic

.PHONY: all check benchmark report clean
all: build/demo build/test_runtime build/benchmark
build:
	mkdir -p build
build/demo: src/uthread.c examples/demo.c include/uthread.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/uthread.c examples/demo.c -o $@
build/test_runtime: src/uthread.c tests/test_runtime.c include/uthread.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/uthread.c tests/test_runtime.c -o $@
build/benchmark: src/uthread.c examples/benchmark.c include/uthread.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread src/uthread.c examples/benchmark.c -o $@
check: all
	python3 scripts/run_experiments.py
benchmark: all
	python3 scripts/benchmark.py
report:
	python3 scripts/make_report.py
clean:
	rm -f build/demo build/test_runtime build/benchmark
