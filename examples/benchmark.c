#define _POSIX_C_SOURCE 200809L
#include "uthread.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Same fixed computation and synchronized marker recorder in all three modes.
 * This is an instrumented experiment, not a context-switch microbenchmark. */
#define EVENT_CAPACITY 10016
struct event { int64_t us; int tid; const char *detail; };
static struct event events[EVENT_CAPACITY];
static size_t event_count;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static int read_fd, mode, steps;
static atomic_int failed;
static unsigned long iterations;
static uint64_t checksum;
static int64_t started;

static int64_t micros(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) abort();
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
static void marker(int tid, const char *detail) {
    if (pthread_mutex_lock(&log_lock)) abort();
    if (event_count == EVENT_CAPACITY) abort();
    events[event_count++] = (struct event){micros() - started, tid, detail};
    if (pthread_mutex_unlock(&log_lock)) abort();
}
static void reader(void *arg) {
    (void)arg;
    char byte;
    marker(0, "read_begin");
    ssize_t n;
    if (mode == 1) n = ut_read(read_fd, &byte, 1, 10000);
    else do { n = read(read_fd, &byte, 1); } while (n < 0 && errno == EINTR);
    if (n != 1 || byte != 'X') failed = 1;
    marker(0, "read_end");
}
static void worker(void *arg) {
    (void)arg;
    uint64_t state = UINT64_C(0x123456789abcdef);
    for (int s = 0; s < steps; s++) {
        marker(1, "worker_tick");
        for (unsigned long j = 0; j < iterations; j++) {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
        }
        if (mode != 2) ut_yield();
    }
    checksum = state;
    marker(1, "worker_done");
}
static void *pthread_reader(void *arg) { reader(arg); return NULL; }
static void *pthread_worker(void *arg) { worker(arg); return NULL; }
static long number(const char *value, long minimum, long maximum) {
    char *end;
    errno = 0;
    long n = strtol(value, &end, 10);
    if (errno || *end || n < minimum || n > maximum) {
        fprintf(stderr, "invalid numeric argument: %s\n", value);
        exit(2);
    }
    return n;
}
int main(int argc, char **argv) {
    if (argc != 6 || (strcmp(argv[1], "blocking") && strcmp(argv[1], "event") && strcmp(argv[1], "pthread"))) {
        fprintf(stderr,"usage: %s blocking|event|pthread STEPS ITERATIONS DELAY_MS TRACE.csv\n",argv[0]);
        return 2;
    }
    mode = !strcmp(argv[1],"event") ? 1 : !strcmp(argv[1],"pthread") ? 2 : 0;
    steps = (int)number(argv[2], 1, 10000);
    iterations = (unsigned long)number(argv[3], 1, 10000000);
    long delay_ms = number(argv[4], 1, 5000);
    int pipes[2];
    if (pipe(pipes)) { perror("pipe"); return 1; }
    started = micros();
    /* Fork before creating any pthreads. The writer never uses the logger. */
    pid_t child = fork();
    if (child < 0) { perror("fork"); close(pipes[0]); close(pipes[1]); return 1; }
    if (!child) {
        close(pipes[0]);
        struct timespec wait = {delay_ms/1000, (delay_ms%1000)*1000000};
        while (nanosleep(&wait,&wait) && errno == EINTR) {}
        char byte = 'X'; ssize_t n;
        do { n = write(pipes[1],&byte,1); } while (n < 0 && errno == EINTR);
        close(pipes[1]);
        _exit(n == 1 ? 0 : 1);
    }
    close(pipes[1]); read_fd = pipes[0];
    if (mode == 1 && fcntl(read_fd,F_SETFL,O_NONBLOCK) < 0) failed = 1;
    int runtime_initialized = 0;
    if (!failed && mode == 2) {
        pthread_t r,w;
        int rc = pthread_create(&r,NULL,pthread_reader,NULL);
        if (rc) failed = 1;
        else {
            int wc = pthread_create(&w,NULL,pthread_worker,NULL);
            if (wc) failed = 1;
            if (pthread_join(r,NULL)) failed = 1;
            if (!wc && pthread_join(w,NULL)) failed = 1;
        }
    } else if (!failed) {
        if (ut_init(UT_ROUND_ROBIN)) failed = 1;
        else {
            runtime_initialized = 1;
            if (ut_spawn(reader,NULL) < 0 || ut_spawn(worker,NULL) < 0 || ut_run()) failed = 1;
        }
    }
    close(read_fd);
    int child_status = 0; pid_t waited;
    do { waited = waitpid(child,&child_status,0); } while (waited < 0 && errno == EINTR);
    if (waited < 0 || !WIFEXITED(child_status) || WEXITSTATUS(child_status)) failed = 1;
    int64_t wall_us = micros() - started;
    struct rusage usage;
    if (getrusage(RUSAGE_SELF,&usage)) return 1;
    /* Export traces after both wall time and CPU usage have been captured. */
    if (runtime_initialized) {
        size_t length = strlen(argv[5]) + 20;
        char *runtime_path = malloc(length);
        if (!runtime_path) failed = 1;
        else {
            snprintf(runtime_path,length,"%s.runtime.csv",argv[5]);
            if (ut_trace_write(runtime_path)) failed = 1;
            free(runtime_path);
        }
        ut_destroy();
    }
    if (failed) { fprintf(stderr,"benchmark failed\n"); return 1; }
    FILE *trace = fopen(argv[5],"w");
    if (!trace) { perror("trace"); return 1; }
    fprintf(trace,"time_us,tid,event,detail\n");
    int64_t read_end = -1, first_tick = -1;
    for (size_t i = 0; i < event_count; i++) {
        struct event *e = &events[i];
        fprintf(trace,"%" PRId64 ",%d,mark,%s\n",e->us,e->tid,e->detail);
        if (!strcmp(e->detail,"read_end")) read_end = e->us;
        if (!strcmp(e->detail,"worker_tick") && first_tick < 0) first_tick = e->us;
    }
    int error = ferror(trace);
    if (fclose(trace) || error || first_tick < 0 || read_end < 0) return 1;
    int before = 0;
    for (size_t i = 0; i < event_count; i++)
        before += !strcmp(events[i].detail,"worker_tick") && events[i].us < read_end;
    long long cpu_us = (long long)usage.ru_utime.tv_sec*1000000 + usage.ru_utime.tv_usec
                     + (long long)usage.ru_stime.tv_sec*1000000 + usage.ru_stime.tv_usec;
    printf("{\"mode\":\"%s\",\"steps\":%d,\"iterations\":%lu,\"delay_ms\":%ld,"
           "\"wall_us\":%" PRId64 ",\"cpu_us\":%lld,\"max_rss_kb\":%ld,"
           "\"first_worker_tick_us\":%" PRId64 ",\"read_end_us\":%" PRId64 ","
           "\"ticks_before_read_end\":%d,\"checksum\":\"%016" PRIx64 "\"}\n",
           argv[1],steps,iterations,delay_ms,wall_us,cpu_us,usage.ru_maxrss,
           first_tick,read_end,before,checksum);
    return 0;
}
