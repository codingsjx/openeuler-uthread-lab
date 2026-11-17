#define _POSIX_C_SOURCE 200809L
#include "uthread.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#ifdef NDEBUG
#error Tests require assertions
#endif

static int pair[2], writer_done, reader_done, union_seen, read_only_seen;
static size_t filled;
static int fallback_once;
int __real_accept4(int, struct sockaddr *, socklen_t *, int);
int __wrap_accept4(int fd, struct sockaddr *address, socklen_t *length, int flags) {
    if (fallback_once) { fallback_once = 0; errno = ENOSYS; return -1; }
    return __real_accept4(fd, address, length, flags);
}
int __real_poll(struct pollfd *, nfds_t, int);
int __real___poll_chk(struct pollfd *, nfds_t, int, size_t);
static void inspect(struct pollfd *fds, nfds_t count) {
    assert(count == 1); /* read and write waiters share one descriptor */
    if (fds[0].events == (POLLIN | POLLOUT)) union_seen = 1;
    if (writer_done && !reader_done) {
        assert(fds[0].events == POLLIN);
        read_only_seen = 1;
    }
}
int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout) {
    inspect(fds, count);
    return __real_poll(fds, count, timeout);
}
int __wrap___poll_chk(struct pollfd *fds, nfds_t count, int timeout, size_t bytes) {
    inspect(fds, count);
    return __real___poll_chk(fds, count, timeout, bytes);
}
static void nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL);
    assert(flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
}
static void setup(void) {
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    int capacity = 4096;
    assert(setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &capacity, sizeof capacity) == 0);
    nonblock(pair[0]); nonblock(pair[1]);
    assert(ut_init_ex(UT_ROUND_ROBIN, UT_TRACE_OFF) == 0);
}
static void cleanup(void) { ut_destroy(); close(pair[0]); close(pair[1]); }
static void fill(void) {
    char bytes[4096] = {0};
    ssize_t n;
    filled = 0;
    while ((n = write(pair[0], bytes, sizeof bytes)) > 0) filled += (size_t)n;
    assert(n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK));
}
static void waiting_reader(void *arg) {
    (void)arg; char byte;
    assert(ut_read(pair[0], &byte, 1, 3000) == 1 && byte == 'R');
    reader_done = 1;
}
static void waiting_writer(void *arg) {
    (void)arg;
    assert(ut_write(pair[0], "W", 1, 3000) == 1);
    writer_done = 1;
}
static void peer(void *arg) {
    (void)arg; char bytes[4096]; ssize_t n;
    size_t drained = 0;
    while ((n = read(pair[1], bytes, sizeof bytes)) > 0) drained += (size_t)n;
    assert(drained == filled && n == -1 && errno == EAGAIN);
    while (!writer_done) ut_yield();
    assert(write(pair[1], "R", 1) == 1);
}
static void timeout_writer(void *arg) {
    (void)arg;
    assert(ut_write(pair[0], "X", 1, 0) == -1 && errno == ETIMEDOUT);
    assert(ut_write(pair[0], "X", 1, 5) == -1 && errno == ETIMEDOUT);
}
static void partial_writer(void *arg) {
    /* Two 64 KiB scratch buffers do not fit on a 128 KiB coroutine stack. */
    static char bytes[65536], received[65536];
    (void)arg; memset(bytes, 'P', sizeof bytes);
    ssize_t n = ut_write(pair[0], bytes, sizeof bytes, 1000);
    assert(n > 0 && n < (ssize_t)sizeof bytes);
    ssize_t count = read(pair[1], received, sizeof received);
    assert(count == n);
    for (ssize_t i = 0; i < n; i++) assert(received[i] == 'P');
}
static void closed_writer(void *arg) {
    (void)arg;
    assert(ut_write(pair[0], "X", 1, 100) == -1 && errno == EPIPE);
}
static void accept_connection(void *arg) {
    int listener = *(int *)arg;
    for (int i = 0; i < 2; i++) {
        int fd = ut_accept(listener, NULL, NULL, 1000);
        assert(fd >= 0);
        assert(fcntl(fd, F_GETFL) & O_NONBLOCK);
        assert(fcntl(fd, F_GETFD) & FD_CLOEXEC);
        assert(close(fd) == 0);
    }
    assert(ut_accept(listener, NULL, NULL, 0) == -1 && errno == ETIMEDOUT);
}
int main(void) {
    alarm(20);
    assert(signal(SIGPIPE, SIG_IGN) != SIG_ERR);
    assert(ut_write(-1, "X", 1, 0) == -1 && errno == EINVAL);
    setup(); fill();
    assert(ut_spawn(waiting_reader, NULL) >= 0);
    assert(ut_spawn(waiting_writer, NULL) >= 0);
    assert(ut_spawn(peer, NULL) >= 0);
    assert(ut_run() == 0 && reader_done && writer_done && union_seen && read_only_seen);
    struct ut_stats stats;
    assert(ut_get_stats(&stats) == 0);
    /* Each task starts once and resumes once. Waking the reader on POLLOUT
     * would add a spurious read/EAGAIN/dispatch round. */
    assert(stats.dispatches == 6);
    cleanup();
    writer_done = reader_done = 0;
    setup(); fill();
    assert(ut_spawn(timeout_writer, NULL) >= 0 && ut_run() == 0);
    cleanup();
    setup();
    assert(ut_spawn(partial_writer, NULL) >= 0 && ut_run() == 0);
    cleanup();
    setup(); close(pair[1]);
    assert(ut_spawn(closed_writer, NULL) >= 0 && ut_run() == 0);
    ut_destroy(); close(pair[0]);
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0); nonblock(listener);
    struct sockaddr_in address = {.sin_family=AF_INET, .sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    assert(bind(listener, (struct sockaddr *)&address, sizeof address) == 0);
    assert(listen(listener, 2) == 0);
    socklen_t length = sizeof address;
    assert(getsockname(listener, (struct sockaddr *)&address, &length) == 0);
    int client = socket(AF_INET, SOCK_STREAM, 0);
    assert(client >= 0 && connect(client, (struct sockaddr *)&address, length) == 0);
    int another = socket(AF_INET, SOCK_STREAM, 0);
    assert(another >= 0 && connect(another, (struct sockaddr *)&address, length) == 0);
    fallback_once = 1;
    assert(ut_init(UT_ROUND_ROBIN) == 0);
    assert(ut_spawn(accept_connection, &listener) >= 0 && ut_run() == 0);
    ut_destroy(); close(client); close(another); close(listener);
    puts("PASS: mixed read/write interests, partial writes, timeout, EPIPE and accept flags");
    return 0;
}
