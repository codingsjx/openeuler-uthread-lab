#define _POSIX_C_SOURCE 200809L
#include "uthread.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum { CYCLES = 2000, READERS = 8 };
static int pipes[READERS][2], ids[READERS], failed;
static unsigned received;

static int fd_count(void) {
    DIR *directory = opendir("/proc/self/fd");
    if (!directory) return -1;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)))
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) count++;
    if (closedir(directory)) return -1;
    return count;
}
static void reader(void *arg) {
    int id = *(int *)arg;
    unsigned char byte;
    if (ut_read(pipes[id][0], &byte, 1, 1000) != 1 || byte != (unsigned char)id)
        failed = 1;
    else received++;
    if (ut_read(pipes[id][0], &byte, 1, 1000) != 0) failed = 1;
}
static void writer(void *arg) {
    (void)arg;
    for (int i = 0; i < READERS; i++) {
        unsigned char byte = (unsigned char)i;
        ssize_t n;
        do { n = write(pipes[i][1], &byte, 1); } while (n < 0 && errno == EINTR);
        if (n != 1 || close(pipes[i][1])) failed = 1;
        ut_yield();
    }
}
int main(void) {
    alarm(60);
    int before = fd_count(), after = -1;
    if (before < 0) return 1;
    for (int cycle = 0; cycle < CYCLES; cycle++) {
        if (ut_init_ex(UT_ROUND_ROBIN, cycle % 2 ? UT_TRACE_OFF : UT_TRACE_FULL)) return 1;
        for (int i = 0; i < READERS; i++) {
            if (pipe(pipes[i])) return 1;
            int flags = fcntl(pipes[i][0], F_GETFL);
            if (flags < 0 || fcntl(pipes[i][0], F_SETFL, flags | O_NONBLOCK)) return 1;
            ids[i] = i;
            if (ut_spawn(reader, &ids[i]) < 0) return 1;
        }
        if (ut_spawn(writer, NULL) < 0 || ut_run()) return 1;
        struct ut_stats stats;
        if (ut_get_stats(&stats) || stats.dropped_events ||
            (cycle % 2 && stats.trace_allocated_bytes)) return 1;
        ut_destroy();
        for (int i = 0; i < READERS; i++) if (close(pipes[i][0])) return 1;
        after = fd_count();
        if (failed || after != before) return 1;
    }
    if (received != CYCLES * READERS) return 1;
    printf("{\"status\":\"passed\",\"cycles\":%d,\"responses\":%u,"
           "\"fd_before\":%d,\"fd_after\":%d,\"trace_modes\":[\"full\",\"off\"]}\n",
           CYCLES, received, before, after);
    return 0;
}
