#define _POSIX_C_SOURCE 200809L
#include "uthread.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Bounded streaming echo, deliberately not HTTP. Drain the pending buffer
 * before reading again: a slow receiver cannot grow an application queue. */
enum { BUFFER_BYTES = 4096, TURN_BYTES = 16384, CONNECTION_SLOTS = 31 };
static int listener, workers = 8, idle_ms = 500, lifetime_ms = 5000, server_failed;
static unsigned long limit, accepted, admitted, rejected, capacity_rejected, resource_rejected;
static unsigned long completed, timeouts, peer_errors, shutdown_closed, active, peak_active;
static volatile sig_atomic_t stopping;
struct connection { int fd; unsigned long id; };
static struct connection connections[CONNECTION_SLOTS];

static void stop_handler(int number) { (void)number; stopping = 1; }
static int64_t now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) abort();
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static int remaining(int64_t deadline) {
    int64_t left = deadline - now_ms();
    if (left <= 0) { errno = ETIMEDOUT; return -1; }
    return left > idle_ms ? idle_ms : (int)left;
}
static void connection_mark(const char *reason, unsigned long id) {
    char label[48];
    snprintf(label, sizeof label, "%s:%lu", reason, id);
    ut_mark(label);
}
static int echo_connection(int fd, unsigned long id) {
    unsigned char buffer[BUFFER_BYTES];
    size_t turn = 0;
    int64_t deadline = now_ms() + lifetime_ms;
    const char *timeout_reason = "read_timeout";
    connection_mark("connection_open", id);
    while (!stopping) {
        timeout_reason = "read_timeout";
        int wait = remaining(deadline);
        if (wait < 0) goto timeout;
        ssize_t count = ut_read(fd, buffer, sizeof buffer, wait);
        if (!count) { connection_mark("peer_eof", id); return 0; }
        if (count < 0) {
            if (errno == ETIMEDOUT) goto timeout;
            connection_mark("read_error", id); return -1;
        }
        size_t offset = 0;
        timeout_reason = "write_timeout";
        /* This deadline is not restarted by each successful partial write. */
        int64_t write_deadline = now_ms() + idle_ms;
        if (write_deadline > deadline) write_deadline = deadline;
        while (offset < (size_t)count && !stopping) {
            wait = remaining(write_deadline);
            if (wait < 0) goto timeout;
            ssize_t n = ut_write(fd, buffer + offset, (size_t)count - offset, wait);
            if (n <= 0) {
                if (n < 0 && errno == ETIMEDOUT) goto timeout;
                connection_mark("write_error", id); return -1;
            }
            offset += (size_t)n;
            turn += (size_t)n;
            if (turn >= TURN_BYTES) {
                turn = 0;
                connection_mark("budget_yield", id);
                ut_yield();
            }
        }
    }
    connection_mark("shutdown", id);
    return 2;
timeout:
    if (stopping) { connection_mark("shutdown", id); return 2; }
    connection_mark(now_ms() >= deadline ? "lifetime_timeout" : timeout_reason, id);
    return 1;
}
static void serve_connection(void *arg) {
    struct connection *connection = arg;
    int status = echo_connection(connection->fd, connection->id);
    if (status == 1) timeouts++;
    else if (status == 2) shutdown_closed++;
    else if (status < 0) peer_errors++;
    else completed++;
    close(connection->fd);
    connection->fd = -1;
    active--;
    connection_mark("connection_closed", connection->id);
    /* Do not yield after releasing the application slot: return first so the
     * scheduler also frees this task stack before the acceptor reuses it. */
}
static void accept_connections(void *arg) {
    (void)arg;
    while (!stopping && (!limit || accepted < limit)) {
        int fd = ut_accept(listener, NULL, NULL, 100);
        if (fd < 0) {
            if (errno == ETIMEDOUT) continue;
            if (errno == ECONNABORTED || errno == EPROTO) { ut_yield(); continue; }
            perror("ut_accept"); server_failed = 1; stopping = 1; return;
        }
        unsigned long id = ++accepted;
        if (stopping) { rejected++; close(fd); break; }
        int slot = 0;
        while (slot < workers && connections[slot].fd >= 0) slot++;
        if (slot == workers) {
            rejected++; capacity_rejected++;
            connection_mark("capacity_reject", id);
            close(fd);
            ut_yield();
            continue;
        }
        /* Kernel socket queues are finite too; their effective size is OS
         * controlled and is separate from the strict application buffer bound. */
        int bytes = 16384;
        if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof bytes)) {
            rejected++; resource_rejected++;
            connection_mark("resource_reject", id);
            close(fd);
        } else {
            connections[slot] = (struct connection){fd, id};
            if (ut_spawn(serve_connection, &connections[slot]) < 0) {
                rejected++; resource_rejected++;
                connection_mark("resource_reject", id);
                connections[slot].fd = -1;
                close(fd);
            } else {
                admitted++; active++;
                if (active > peak_active) peak_active = active;
                connection_mark("connection_admitted", id);
            }
        }
        /* Bound the acceptor's own work even under a continuous connection flood. */
        ut_yield();
    }
}
static long number(const char *text, long minimum, long maximum) {
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno || end == text || *end || value < minimum || value > maximum) {
        fprintf(stderr, "invalid argument: %s\n", text); exit(2);
    }
    return value;
}
int main(int argc, char **argv) {
    if (argc > 7) {
        fprintf(stderr, "usage: %s [PORT [MAX_CONNECTIONS [WORKERS [IDLE_MS [LIFETIME_MS [TRACE.csv]]]]]]\n", argv[0]);
        return 2;
    }
    int port = argc > 1 ? (int)number(argv[1], 0, 65535) : 8080;
    limit = argc > 2 ? (unsigned long)number(argv[2], 0, 1000000) : 0;
    if (argc > 3) workers = (int)number(argv[3], 1, CONNECTION_SLOTS);
    if (argc > 4) idle_ms = (int)number(argv[4], 1, 60000);
    if (argc > 5) lifetime_ms = (int)number(argv[5], 1, 3600000);
    struct sigaction stop = {0}, ignore = {0};
    stop.sa_handler = stop_handler; ignore.sa_handler = SIG_IGN;
    sigemptyset(&stop.sa_mask); sigemptyset(&ignore.sa_mask);
    if (sigaction(SIGINT, &stop, NULL) || sigaction(SIGTERM, &stop, NULL) ||
        sigaction(SIGPIPE, &ignore, NULL)) return 1;
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) { perror("socket"); return 1; }
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int flags = fcntl(listener, F_GETFL), descriptor_flags = fcntl(listener, F_GETFD);
    if (flags < 0 || descriptor_flags < 0 ||
        fcntl(listener, F_SETFL, flags | O_NONBLOCK) ||
        fcntl(listener, F_SETFD, descriptor_flags | FD_CLOEXEC) ||
        bind(listener, (struct sockaddr *)&address, sizeof address) || listen(listener, 16)) {
        perror("listener setup"); close(listener); return 1;
    }
    socklen_t length = sizeof address;
    if (getsockname(listener, (struct sockaddr *)&address, &length)) return 1;
    if (ut_init_ex(UT_ROUND_ROBIN, argc > 6 ? UT_TRACE_RECENT : UT_TRACE_OFF)) return 1;
    for (int i = 0; i < CONNECTION_SLOTS; i++) connections[i].fd = -1;
    if (ut_spawn(accept_connections, NULL) < 0) { ut_destroy(); close(listener); return 1; }
    printf("{\"event\":\"listening\",\"address\":\"127.0.0.1\",\"port\":%u,"
           "\"workers\":%d,\"buffer_bytes_per_connection\":%d,"
           "\"admission\":\"close_when_full\",\"trace_mode\":\"%s\"}\n",
           (unsigned)ntohs(address.sin_port), workers, BUFFER_BYTES, argc > 6 ? "recent" : "off");
    fflush(stdout);
    int status = ut_run();
    if (argc > 6 && ut_trace_write(argv[6])) { perror("trace export"); status = -1; }
    /* ut_run can fail before task callbacks unwind; keep fd ownership here. */
    for (int i = 0; i < workers; i++) {
        if (connections[i].fd >= 0) {
            close(connections[i].fd);
            connections[i].fd = -1;
            active--; shutdown_closed++;
        }
    }
    ut_destroy(); close(listener);
    printf("{\"event\":\"stopped\",\"accepted\":%lu,\"completed\":%lu,"
           "\"admitted\":%lu,\"rejected\":%lu,\"capacity_rejected\":%lu,"
           "\"resource_rejected\":%lu,\"timeouts\":%lu,\"peer_errors\":%lu,"
           "\"shutdown_closed\":%lu,\"peak_active\":%lu,\"active_remaining\":%lu}\n",
           accepted, completed, admitted, rejected, capacity_rejected, resource_rejected,
           timeouts, peer_errors, shutdown_closed, peak_active, active);
    return status || server_failed ? 1 : 0;
}
