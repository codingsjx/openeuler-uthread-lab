#define _POSIX_C_SOURCE 200809L
#include "uthread.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Bounded, local request/reply workload. One request is outstanding per client;
 * replies are one byte, so this does not implement general asynchronous writes. */
enum { CLIENTS = 4, REQUESTS = 8 };
static int channels[CLIENTS][2], event_mode, failed, slow_received;
static unsigned completed, fast_before_slow;
static int64_t started, first_reply[CLIENTS];

static int64_t micros(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) abort();
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
static ssize_t read_byte(int fd, unsigned char *byte) {
    ssize_t n;
    do { n = read(fd, byte, 1); } while (n < 0 && errno == EINTR);
    return n;
}
static int write_byte(int fd, unsigned char byte) {
    ssize_t n;
    do { n = write(fd, &byte, 1); } while (n < 0 && errno == EINTR);
    return n == 1 ? 0 : -1;
}
static void serve(void *arg) {
    int id = *(int *)arg;
    for (int sequence = 0; sequence < REQUESTS; sequence++) {
        unsigned char request;
        ssize_t n = event_mode ? ut_read(channels[id][0], &request, 1, 5000)
                               : read_byte(channels[id][0], &request);
        if (n != 1 || request != (unsigned char)(id * REQUESTS + sequence)) {
            failed = 1;
            return;
        }
        if (!id) slow_received = 1;
        if (write_byte(channels[id][0], (unsigned char)(request ^ 0x5a))) {
            failed = 1;
            return;
        }
        if (!sequence) first_reply[id] = micros() - started;
        if (id && !slow_received) fast_before_slow++;
        completed++;
        ut_yield();
    }
}
static void client(int id, int gate) {
    /* Children inherit only their endpoint, plus the common start gate. */
    for (int i = 0; i < CLIENTS; i++) {
        close(channels[i][0]);
        if (i != id) close(channels[i][1]);
    }
    unsigned char byte;
    if (read_byte(gate, &byte) != 1) _exit(2);
    close(gate);
    if (!id) {
        struct timespec delay = {0, 300000000};
        while (nanosleep(&delay, &delay) && errno == EINTR) {}
    }
    for (int sequence = 0; sequence < REQUESTS; sequence++) {
        unsigned char request = (unsigned char)(id * REQUESTS + sequence);
        if (write_byte(channels[id][1], request) ||
            read_byte(channels[id][1], &byte) != 1 ||
            byte != (unsigned char)(request ^ 0x5a)) _exit(3);
    }
    close(channels[id][1]);
    _exit(0);
}
int main(int argc, char **argv) {
    if (argc != 3 || (strcmp(argv[1], "blocking") && strcmp(argv[1], "event"))) {
        fprintf(stderr, "usage: %s blocking|event TRACE.csv\n", argv[0]);
        return 2;
    }
    /* The controller also has a process-group timeout; alarm bounds standalone runs. */
    alarm(15);
    event_mode = !strcmp(argv[1], "event");
    int gate[2], ids[CLIENTS];
    pid_t children[CLIENTS];
    if (pipe(gate)) return 1;
    for (int i = 0; i < CLIENTS; i++)
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, channels[i])) return 1;
    for (int i = 0; i < CLIENTS; i++) {
        children[i] = fork();
        if (children[i] < 0) return 1;
        if (!children[i]) { close(gate[1]); client(i, gate[0]); }
    }
    close(gate[0]);
    if (ut_init(UT_ROUND_ROBIN)) return 1;
    for (int i = 0; i < CLIENTS; i++) {
        close(channels[i][1]);
        if (event_mode) {
            int flags = fcntl(channels[i][0], F_GETFL);
            if (flags < 0 || fcntl(channels[i][0], F_SETFL, flags | O_NONBLOCK)) return 1;
        }
        ids[i] = i;
        if (ut_spawn(serve, &ids[i]) < 0) return 1;
    }
    started = micros();
    for (int i = 0; i < CLIENTS; i++) if (write_byte(gate[1], 'G')) return 1;
    close(gate[1]);
    if (ut_run() || ut_trace_write(argv[2])) failed = 1;
    int64_t elapsed = micros() - started;
    ut_destroy();
    for (int i = 0; i < CLIENTS; i++) {
        close(channels[i][0]);
        int status;
        pid_t waited;
        do { waited = waitpid(children[i], &status, 0); } while (waited < 0 && errno == EINTR);
        if (waited != children[i] || !WIFEXITED(status) || WEXITSTATUS(status)) failed = 1;
    }
    if (completed != CLIENTS * REQUESTS) failed = 1;
    printf("{\"mode\":\"%s\",\"status\":\"%s\",\"responses\":%u,"
           "\"fast_responses_before_slow_request\":%u,\"elapsed_us\":%lld,"
           "\"first_reply_us\":[%lld,%lld,%lld,%lld]}\n", argv[1],
           failed ? "failed" : "passed", completed, fast_before_slow,
           (long long)elapsed, (long long)first_reply[0], (long long)first_reply[1],
           (long long)first_reply[2], (long long)first_reply[3]);
    return failed ? 1 : 0;
}
