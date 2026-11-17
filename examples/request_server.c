#define _POSIX_C_SOURCE 200809L
#include "uthread.h"
#include "request_policy.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { MAX_BODY = 4096, MAX_SLOTS = 31 };
enum { IO_OK, IO_EOF, IO_TRUNCATED, IO_TIMEOUT, IO_CANCEL, IO_ERROR };
enum role { ROLE_WORKER, ROLE_PROBE };
enum outcome { CLOSED_CLEAN, CLOSED_TIMEOUT, CLOSED_ERROR, CLOSED_PROTOCOL, CLOSED_RECLAIMED,
               CLOSED_SHUTDOWN, CLOSED_REJECTED };
struct connection {
    int fd, evicted;
    enum role role;
    unsigned long id;
    struct ut_task_handle task;
    struct request_progress progress;
    int64_t accepted_ms, lifetime_deadline, stage_deadline;
    unsigned operations;
    unsigned char header[4], body[MAX_BODY];
};
static struct connection *connections;
static int capacity = 8, probe_capacity = 2, header_ms = 1000, body_ms = 1000;
static int response_ms = 1000, idle_ms = 2000, lifetime_ms = 10000, stall_ms = 200;
static int listener = -1, notifications[2] = {-1, -1}, failed;
static unsigned long limit, accepted, opened, closed_total, direct_rejected, probe_rejected;
static unsigned long direct_admitted, probe_started, promoted, requests_completed;
static unsigned long clean_closed, timeout_closed, protocol_closed, error_closed, reclaimed, shutdown_closed;
static unsigned long phase_timeouts[4], probe_timeouts, lifetime_timeouts;
static int live_workers, live_probes, reservations, peak_workers, peak_probes, peak_tasks = 1, peak_fds = 3;
static volatile sig_atomic_t stopping;
static enum request_admission policy = ADMISSION_PHASE_AWARE;
static const char *policy_name = "phase-aware";

static int64_t now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) abort();
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void signal_stop(int number) { (void)number; stopping = 1; }
static void mark(const char *reason, unsigned long id) {
    char label[48]; snprintf(label, sizeof label, "%s:%lx", reason, id); ut_mark(label);
}
static void phase_mark(const char *reason, struct connection *connection) {
    char label[48];
    snprintf(label, sizeof label, "%s:%lx:%s", reason, connection->id,
             request_phase_name(connection->progress.phase));
    ut_mark(label);
}
static void begin_phase(struct connection *connection, enum request_phase phase, int64_t start, int budget) {
    connection->progress.phase = phase;
    connection->progress.stage_started_ms = start;
    connection->stage_deadline = start + budget;
    if (connection->stage_deadline > connection->lifetime_deadline)
        connection->stage_deadline = connection->lifetime_deadline;
    phase_mark("phase", connection);
}
static int cancelled(void) { return stopping || ut_cancelled() == 1; }
static int transfer(struct connection *connection, unsigned char *data, size_t size, int writing) {
    size_t offset = 0;
    while (offset < size) {
        if (cancelled()) return IO_CANCEL;
        int64_t left = connection->stage_deadline - now_ms();
        if (left <= 0) return IO_TIMEOUT;
        ssize_t count = writing ? ut_write(connection->fd, data + offset, size - offset, (int)left)
                                : ut_read(connection->fd, data + offset, size - offset, (int)left);
        if (count < 0) {
            if (errno == ECANCELED) return IO_CANCEL;
            if (errno == ETIMEDOUT) return IO_TIMEOUT;
            return IO_ERROR;
        }
        if (!count) return writing ? IO_ERROR : offset ? IO_TRUNCATED : IO_EOF;
        offset += (size_t)count;
        connection->progress.last_progress_ms = now_ms();
        if (!writing) connection->progress.received_bytes += (unsigned long long)count;
        /* The stage deadline never moves when a byte or partial write succeeds. */
        if (now_ms() >= connection->stage_deadline) return IO_TIMEOUT;
        if (++connection->operations >= 8 && offset < size) {
            connection->operations = 0; mark("operation_yield", connection->id); ut_yield();
        }
    }
    return IO_OK;
}
static int eligible_victim(void) {
    int choice = -1;
    int64_t now = now_ms();
    for (int i = 0; i < capacity + probe_capacity; i++) {
        struct connection *candidate = &connections[i];
        struct ut_task_info info;
        if (candidate->fd < 0 || candidate->role != ROLE_WORKER || candidate->evicted ||
            !request_reclaimable(policy, &candidate->progress, now, stall_ms) ||
            ut_get_task_info(candidate->task, &info) || info.cancelled || info.state != UT_TASK_WAIT_IO) continue;
        struct pollfd readiness = {.fd=candidate->fd, .events=info.events};
        if (poll(&readiness, 1, 0) != 0) continue;
        if (choice < 0 || candidate->progress.stage_started_ms < connections[choice].progress.stage_started_ms)
            choice = i;
    }
    return choice;
}
static int admit_probe(struct connection *connection) {
    if (live_workers + reservations < capacity) goto promote;
    int index = eligible_victim();
    if (index < 0) return 0;
    struct connection *victim = &connections[index];
    struct ut_task_handle old_task = victim->task;
    unsigned long old_id = victim->id;
    phase_mark("victim_phase", victim);
    victim->evicted = 1;
    reservations++;
    if (ut_cancel(old_task)) { victim->evicted = 0; reservations--; return 0; }
    char label[48];
    snprintf(label, sizeof label, "reclaim:%lx>%lx", old_id, connection->id); ut_mark(label);
    /* The acceptor can reuse the application slot while this probe yields.
     * Wait on the generation-checked task, never on a reused fd/slot number. */
    for (;;) {
        struct ut_task_info info;
        if (ut_get_task_info(old_task, &info) || info.state == UT_TASK_DONE) break;
        ut_yield();
    }
    reservations--;
    if (cancelled()) return -1;
promote:
    live_probes--; live_workers++; promoted++;
    if (live_workers > peak_workers) peak_workers = live_workers;
    connection->role = ROLE_WORKER;
    mark("probe_promoted", connection->id);
    return 1;
}
static enum outcome io_outcome(int result, struct connection *connection) {
    if (connection->evicted) return CLOSED_RECLAIMED;
    if (result == IO_CANCEL || stopping) return CLOSED_SHUTDOWN;
    if (result == IO_TIMEOUT) {
        phase_timeouts[connection->progress.phase]++;
        if (connection->role == ROLE_PROBE) probe_timeouts++;
        if (now_ms() >= connection->lifetime_deadline) {
            lifetime_timeouts++; mark("lifetime_timeout", connection->id);
        } else phase_mark("phase_timeout", connection);
        return CLOSED_TIMEOUT;
    }
    if (result == IO_EOF || result == IO_TRUNCATED) return CLOSED_PROTOCOL;
    return CLOSED_ERROR;
}
static enum outcome handle_requests(struct connection *connection) {
    int first = 1;
    for (;;) {
        int result;
        if (cancelled()) return io_outcome(IO_CANCEL, connection);
        if (!first) {
            begin_phase(connection, REQUEST_IDLE, now_ms(), idle_ms);
            result = transfer(connection, connection->header, 1, 0);
            if (result == IO_EOF) return CLOSED_CLEAN;
            if (result != IO_OK) return io_outcome(result, connection);
            begin_phase(connection, REQUEST_HEADER, now_ms(), header_ms);
            result = transfer(connection, connection->header + 1, 3, 0);
        } else {
            begin_phase(connection, REQUEST_HEADER, connection->accepted_ms, header_ms);
            result = transfer(connection, connection->header, 4, 0);
            if (result == IO_EOF) return CLOSED_CLEAN;
        }
        first = 0;
        if (result != IO_OK) return io_outcome(result, connection);
        uint32_t wire_length;
        memcpy(&wire_length, connection->header, sizeof wire_length);
        uint32_t length = ntohl(wire_length);
        if (!length || length > MAX_BODY) { mark("invalid_length", connection->id); return CLOSED_PROTOCOL; }
        begin_phase(connection, REQUEST_BODY, now_ms(), body_ms);
        result = transfer(connection, connection->body, length, 0);
        if (result != IO_OK) return io_outcome(result, connection);
        mark("request_ready", connection->id);
        if (connection->role == ROLE_PROBE) {
            int admitted = admit_probe(connection);
            if (admitted < 0) return CLOSED_SHUTDOWN;
            if (!admitted) { mark("probe_rejected", connection->id); return CLOSED_REJECTED; }
        }
        for (uint32_t i = 0; i < length; i++) connection->body[i] ^= 0x5a;
        begin_phase(connection, REQUEST_WRITE, now_ms(), response_ms);
        result = transfer(connection, connection->header, 4, 1);
        if (result == IO_OK) result = transfer(connection, connection->body, length, 1);
        if (result != IO_OK) return io_outcome(result, connection);
        requests_completed++; mark("request_complete", connection->id);
        connection->operations = 0;
        ut_yield();
    }
}
static void notify_supervisor(void) {
    unsigned char byte = 1;
    ssize_t result;
    do { result = write(notifications[1], &byte, 1); } while (result < 0 && errno == EINTR);
    /* Nonblocking notification is only a hint; supervisor also has a deadline. */
}
static void serve(void *arg) {
    struct connection *connection = arg;
    enum outcome result = handle_requests(connection);
    switch (result) {
        case CLOSED_CLEAN: clean_closed++; break;
        case CLOSED_TIMEOUT: timeout_closed++; break;
        case CLOSED_PROTOCOL: protocol_closed++; break;
        case CLOSED_RECLAIMED: reclaimed++; break;
        case CLOSED_SHUTDOWN: shutdown_closed++; break;
        case CLOSED_REJECTED: probe_rejected++; break;
        default: error_closed++; break;
    }
    close(connection->fd); connection->fd = -1; closed_total++;
    if (connection->role == ROLE_WORKER) live_workers--; else live_probes--;
    mark("connection_closed", connection->id);
    notify_supervisor();
    /* Return before the registry slot or task stack may be reused. */
}
static void update_peaks(void) {
    if (live_workers > peak_workers) peak_workers = live_workers;
    if (live_probes > peak_probes) peak_probes = live_probes;
    if (live_workers + live_probes + 1 > peak_tasks) peak_tasks = live_workers + live_probes + 1;
}
static void supervisor(void *arg) {
    (void)arg;
    while (!stopping && (!limit || accepted < limit)) {
        int fd = ut_accept(listener, NULL, NULL, 100);
        if (fd < 0) {
            if (errno == ETIMEDOUT) continue;
            if (errno == ECONNABORTED || errno == EPROTO) { ut_yield(); continue; }
            perror("accept"); failed = 1; stopping = 1; break;
        }
        unsigned long id = ++accepted;
        int current_fds = 4 + live_workers + live_probes;
        if (current_fds > peak_fds) peak_fds = current_fds;
        enum role role = live_workers + reservations < capacity ? ROLE_WORKER : ROLE_PROBE;
        int slot = 0;
        while (slot < capacity + probe_capacity && connections[slot].fd >= 0) slot++;
        if (stopping || slot == capacity + probe_capacity ||
            (role == ROLE_PROBE && (policy == ADMISSION_REJECT || live_probes >= probe_capacity))) {
            direct_rejected++; mark("capacity_reject", id); close(fd); ut_yield(); continue;
        }
        int bytes = 16384;
        if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof bytes)) {
            direct_rejected++; close(fd); ut_yield(); continue;
        }
        int64_t start = now_ms();
        connections[slot] = (struct connection){.fd=fd, .id=id, .role=role, .accepted_ms=start,
            .lifetime_deadline=start+lifetime_ms, .progress={REQUEST_HEADER,start,start,0}};
        if (ut_spawn_task(serve, &connections[slot], &connections[slot].task) < 0) {
            direct_rejected++; close(fd); connections[slot].fd = -1;
        } else {
            opened++;
            if (role == ROLE_WORKER) { live_workers++; direct_admitted++; mark("connection_admitted", id); }
            else { live_probes++; probe_started++; mark("probe_started", id); }
            update_peaks();
        }
        ut_yield();
    }
    int cancellation_sent = 0;
    while (live_workers || live_probes) {
        if (stopping && !cancellation_sent) {
            for (int i = 0; i < capacity + probe_capacity; i++)
                if (connections[i].fd >= 0) (void)ut_cancel(connections[i].task);
            cancellation_sent = 1;
        }
        unsigned char hints[64];
        (void)ut_read(notifications[0], hints, sizeof hints, 100);
    }
}
static long number(const char *value, long minimum, long maximum) {
    char *end; errno = 0;
    long n = strtol(value, &end, 10);
    if (errno || end == value || *end || n < minimum || n > maximum) {
        fprintf(stderr, "invalid argument: %s\n", value); exit(2);
    }
    return n;
}
static int nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL), descriptor = fcntl(fd, F_GETFD);
    return flags < 0 || descriptor < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) ||
           fcntl(fd, F_SETFD, descriptor | FD_CLOEXEC);
}
int main(int argc, char **argv) {
    int port = 8081;
    const char *trace = NULL;
    for (int i = 1; i < argc; i++) {
        const char *value = strchr(argv[i], '=');
        if (!value) { fprintf(stderr, "use --name=value options\n"); return 2; }
        value++;
        if (!strncmp(argv[i], "--port=", 7)) port = (int)number(value, 0, 65535);
        else if (!strncmp(argv[i], "--limit=", 8)) limit = (unsigned long)number(value, 0, 1000000);
        else if (!strncmp(argv[i], "--capacity=", 11)) capacity = (int)number(value, 1, 31);
        else if (!strncmp(argv[i], "--probes=", 9)) probe_capacity = (int)number(value, 0, 30);
        else if (!strncmp(argv[i], "--header-ms=", 12)) header_ms = (int)number(value, 1, 60000);
        else if (!strncmp(argv[i], "--body-ms=", 10)) body_ms = (int)number(value, 1, 60000);
        else if (!strncmp(argv[i], "--response-ms=", 14)) response_ms = (int)number(value, 1, 60000);
        else if (!strncmp(argv[i], "--idle-ms=", 10)) idle_ms = (int)number(value, 1, 60000);
        else if (!strncmp(argv[i], "--lifetime-ms=", 14)) lifetime_ms = (int)number(value, 1, 3600000);
        else if (!strncmp(argv[i], "--stall-ms=", 11)) stall_ms = (int)number(value, 1, 60000);
        else if (!strncmp(argv[i], "--trace=", 8)) { if (!*value) return 2; trace = value; }
        else if (!strncmp(argv[i], "--admission=", 12)) {
            if (!strcmp(value, "reject")) policy = ADMISSION_REJECT;
            else if (!strcmp(value, "byte-stalled")) policy = ADMISSION_BYTE_STALLED;
            else if (!strcmp(value, "phase-aware")) policy = ADMISSION_PHASE_AWARE;
            else { fprintf(stderr, "unknown policy\n"); return 2; }
            policy_name = value;
        } else { fprintf(stderr, "unknown option: %s\n", argv[i]); return 2; }
    }
    if (capacity + probe_capacity > MAX_SLOTS) { fprintf(stderr, "capacity + probes must be <= 31\n"); return 2; }
    connections = calloc((size_t)(capacity + probe_capacity), sizeof *connections);
    if (!connections) return 1;
    for (int i = 0; i < capacity + probe_capacity; i++) connections[i].fd = -1;
    struct sigaction action = {0}, ignore = {0};
    action.sa_handler = signal_stop; ignore.sa_handler = SIG_IGN;
    sigemptyset(&action.sa_mask); sigemptyset(&ignore.sa_mask);
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL) || sigaction(SIGPIPE, &ignore, NULL)) return 1;
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0 || pipe(notifications) || nonblock(listener) || nonblock(notifications[0]) || nonblock(notifications[1])) return 1;
    int reuse = 1;
    struct sockaddr_in address = {.sin_family=AF_INET, .sin_port=htons((uint16_t)port), .sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse) ||
        bind(listener, (struct sockaddr *)&address, sizeof address) || listen(listener, 16)) { perror("listen"); return 1; }
    socklen_t length = sizeof address;
    if (getsockname(listener, (struct sockaddr *)&address, &length) || ut_init_ex(UT_ROUND_ROBIN, trace ? UT_TRACE_RECENT : UT_TRACE_OFF)) return 1;
    if (ut_spawn(supervisor, NULL) < 0) return 1;
    printf("{\"event\":\"listening\",\"port\":%u,\"protocol\":\"u32be-xor5a\",\"max_body\":%d,"
           "\"policy\":\"%s\",\"capacity\":%d,\"probes\":%d,\"task_budget\":%d,\"fd_budget\":%d,\"buffer_budget\":%zu}\n",
           (unsigned)ntohs(address.sin_port), MAX_BODY, policy_name, capacity, probe_capacity,
           1+capacity+probe_capacity, 4+capacity+probe_capacity, (size_t)(capacity+probe_capacity)*(MAX_BODY+4));
    fflush(stdout);
    if (ut_run()) failed = 1;
    if (trace && ut_trace_write(trace)) { perror("trace"); failed = 1; }
    for (int i = 0; i < capacity + probe_capacity; i++) {
        if (connections[i].fd >= 0) {
            close(connections[i].fd); connections[i].fd = -1; closed_total++; shutdown_closed++;
        }
    }
    ut_destroy(); close(listener); listener = -1;
    close(notifications[0]); notifications[0] = -1;
    close(notifications[1]); notifications[1] = -1;
    int owned_remaining = listener >= 0 || notifications[0] >= 0 || notifications[1] >= 0;
    for (int i = 0; i < capacity + probe_capacity; i++) owned_remaining += connections[i].fd >= 0;
    free(connections);
    if (failed) { live_workers = live_probes = reservations = 0; }
    printf("{\"event\":\"stopped\",\"accepted\":%lu,\"opened\":%lu,\"closed\":%lu,\"direct_rejected\":%lu,"
           "\"probe_rejected\":%lu,\"direct_admitted\":%lu,\"probe_started\":%lu,\"promoted\":%lu,"
           "\"requests_completed\":%lu,\"clean_closed\":%lu,\"timeout_closed\":%lu,\"protocol_closed\":%lu,"
           "\"error_closed\":%lu,\"reclaimed\":%lu,\"shutdown_closed\":%lu,\"header_timeouts\":%lu,"
           "\"body_timeouts\":%lu,\"write_timeouts\":%lu,\"idle_timeouts\":%lu,\"probe_timeouts\":%lu,"
           "\"lifetime_timeouts\":%lu,\"peak_workers\":%d,\"peak_probes\":%d,\"peak_tasks\":%d,\"peak_fds\":%d,"
           "\"workers_remaining\":%d,\"probes_remaining\":%d,\"reservations_remaining\":%d,\"owned_fds_remaining\":%d}\n",
           accepted, opened, closed_total, direct_rejected, probe_rejected, direct_admitted, probe_started, promoted,
           requests_completed, clean_closed, timeout_closed, protocol_closed, error_closed, reclaimed, shutdown_closed,
           phase_timeouts[REQUEST_HEADER], phase_timeouts[REQUEST_BODY], phase_timeouts[REQUEST_WRITE], phase_timeouts[REQUEST_IDLE],
           probe_timeouts, lifetime_timeouts, peak_workers, peak_probes, peak_tasks, peak_fds,
           live_workers, live_probes, reservations, owned_remaining);
    return failed ? 1 : 0;
}
