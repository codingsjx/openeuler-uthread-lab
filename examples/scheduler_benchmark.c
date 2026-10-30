#define _POSIX_C_SOURCE 200809L
#include "uthread.h"
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

static int yields, completed;
static uint64_t checksum;
static int ids[32];
static void task(void *arg) {
    int id = *(int *)arg;
    for (int i = 0; i < yields; i++) {
        checksum += (uint64_t)id + 1;
        ut_yield();
    }
    completed++;
}
static int64_t micros(void) {
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time)) abort();
    return (int64_t)time.tv_sec * 1000000 + time.tv_nsec/1000;
}
static int number(const char *s, int maximum) {
    char *end; errno = 0;
    long value = strtol(s,&end,10);
    if (errno || *end || value < 1 || value > maximum) exit(2);
    return (int)value;
}
static long long cpu_us(const struct rusage *usage) {
    return (long long)usage->ru_utime.tv_sec*1000000 + usage->ru_utime.tv_usec
         + (long long)usage->ru_stime.tv_sec*1000000 + usage->ru_stime.tv_usec;
}
int main(int argc, char **argv) {
    if (argc != 4 || (strcmp(argv[3],"on") && strcmp(argv[3],"off"))) {
        fprintf(stderr,"usage: %s TASKS YIELDS on|off\n",argv[0]); return 2;
    }
    int tasks = number(argv[1],32);
    yields = number(argv[2],1000); /* keep the historical trace below its limit */
    struct rusage before,after;
    if (getrusage(RUSAGE_SELF,&before)) return 1;
    int64_t start = micros();
#ifdef HAVE_TRACE_OPTIONS
    if (ut_init_ex(UT_ROUND_ROBIN,!strcmp(argv[3],"off") ? UT_TRACE_OFF : UT_TRACE_FULL)) return 1;
#else
    if (strcmp(argv[3],"on") || ut_init(UT_ROUND_ROBIN)) return 1;
#endif
    for (int i = 0; i < tasks; i++) {
        ids[i] = i;
        if (ut_spawn(task,&ids[i]) < 0) return 1;
    }
    if (ut_run()) return 1;
    int64_t wall_us = micros() - start;
    if (getrusage(RUSAGE_SELF,&after)) return 1;
    uint64_t expected = (uint64_t)yields*tasks*(tasks+1)/2;
    if (completed != tasks || checksum != expected) return 1;
    printf("{\"tasks\":%d,\"yields\":%d,\"trace\":\"%s\",\"wall_us\":%" PRId64
           ",\"cpu_us\":%lld,\"completed\":%d,\"checksum\":\"%016" PRIx64 "\"",
           tasks,yields,argv[3],wall_us,cpu_us(&after)-cpu_us(&before),completed,checksum);
#ifdef HAVE_TRACE_OPTIONS
    struct ut_stats stats;
    if (ut_get_stats(&stats) || stats.dropped_events) return 1;
    printf(",\"dispatches\":%llu,\"poll_calls\":%llu,\"yield_shortcuts\":%llu,"
           "\"trace_bytes\":%zu,\"events\":%zu",stats.dispatches,stats.poll_calls,
           stats.yield_shortcuts,stats.trace_allocated_bytes,stats.recorded_events);
#endif
    puts("}");
    ut_destroy();
    return 0;
}
