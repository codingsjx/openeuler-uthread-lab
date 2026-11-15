#define _POSIX_C_SOURCE 200809L
#include "uthread.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef NDEBUG
#error Tests require assertions
#endif
static int sockets[2], cancelled, survived;
static struct ut_task_handle victim;
static void cancelled_reader(void *arg) {
    (void)arg; char byte;
    assert(ut_read(sockets[0], &byte, 1, -1) == -1 && errno == ECANCELED);
    assert(ut_cancelled() == 1);
    cancelled++;
}
static void survivor(void *arg) {
    (void)arg; char byte;
    assert(ut_read(sockets[0], &byte, 1, 1000) == 1 && byte == 'X');
    assert(ut_cancelled() == 0);
    survived++;
}
static void controller(void *arg) {
    (void)arg;
    struct ut_task_info info;
    assert(ut_get_task_info(victim, &info) == 0 && info.state == UT_TASK_WAIT_IO);
    assert(ut_cancel(victim) == 0 && ut_cancel(victim) == 0);
    assert(ut_get_task_info(victim, &info) == 0 && info.state == UT_TASK_READY);
    assert(write(sockets[1], "X", 1) == 1);
}
static void ready_cancel(void *arg) {
    (void)arg; char byte;
    assert(ut_cancelled() == 1);
    assert(ut_read(sockets[0], &byte, 1, 0) == -1 && errno == ECANCELED);
    assert(ut_write(sockets[0], "X", 1, 0) == -1 && errno == ECANCELED);
    assert(ut_accept(sockets[0], NULL, NULL, 0) == -1 && errno == ECANCELED);
}
static unsigned overflow(unsigned depth) {
    volatile unsigned char pad[1024];
    pad[depth % sizeof pad] = (unsigned char)depth;
    unsigned value = depth < 10000 ? overflow(depth + 1) : 0;
    return value + pad[depth % sizeof pad];
}
static void overflow_task(void *arg) { (void)arg; volatile unsigned result = overflow(0); (void)result; }
int main(void) {
    alarm(20);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(fcntl(sockets[0], F_SETFL, O_NONBLOCK) == 0);
    assert(ut_init(UT_ROUND_ROBIN) == 0);
    assert(ut_spawn_task(cancelled_reader, NULL, &victim) >= 0);
    assert(ut_spawn(survivor, NULL) >= 0 && ut_spawn(controller, NULL) >= 0);
    assert(ut_run() == 0 && cancelled == 1 && survived == 1);
    struct ut_task_handle stale = victim, next;
    assert(ut_cancel(stale) == -1 && errno == ESRCH);
    assert(ut_spawn_task(ready_cancel, NULL, &next) == stale.id);
    assert(next.generation != stale.generation);
    assert(ut_cancel(stale) == -1 && errno == ESRCH);
    assert(ut_cancel(next) == 0 && ut_run() == 0);
    ut_destroy();
    assert(ut_init(UT_ROUND_ROBIN) == 0);
    assert(ut_spawn_task(survivor, NULL, &next) == stale.id);
    assert(ut_cancel(stale) == -1 && errno == ESRCH);
    assert(write(sockets[1], "X", 1) == 1 && ut_run() == 0 && survived == 2);
    ut_destroy(); close(sockets[0]); close(sockets[1]);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        struct rlimit no_core = {0, 0};
        if (setrlimit(RLIMIT_CORE, &no_core)) _exit(2);
        alarm(3);
        if (ut_init(UT_ROUND_ROBIN) || ut_spawn(overflow_task, NULL) < 0) _exit(3);
        (void)ut_run(); _exit(4);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
    puts("PASS: isolated cancellation, stale handles, reinitialization and stack guard fault");
    return 0;
}
