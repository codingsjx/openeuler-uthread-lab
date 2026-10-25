#define _POSIX_C_SOURCE 200809L
#include "uthread.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int read_fd, event_mode, failed;
static void steps(void *arg) {
    const char *label = arg;
    for (int i = 0; i < 4; i++) { ut_mark(label); ut_yield(); }
}
static void reader(void *unused) {
    (void)unused;
    char c;
    ut_mark("read_begin");
    ssize_t n;
    if (event_mode) n = ut_read(read_fd, &c, 1, 3000);
    else do { n = read(read_fd, &c, 1); } while (n < 0 && errno == EINTR);
    if (n != 1 || c != 'X') { failed = 1; ut_mark("read_error"); }
    else ut_mark("read_end");
}
static long long micros(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
static void worker(void *unused) {
    (void)unused;
    for (int i = 0; i < 40; i++) {
        ut_mark("worker_tick");
        long long end = micros() + 2000;
        while (micros() < end) { /* controlled CPU work, intentionally no I/O */ }
        ut_yield();
    }
}
int main(int argc, char **argv) {
    if (argc != 4 || (strcmp(argv[1],"schedule") && strcmp(argv[1],"io")) ||
        (!strcmp(argv[1],"schedule") && strcmp(argv[2],"first") && strcmp(argv[2],"rr")) ||
        (!strcmp(argv[1],"io") && strcmp(argv[2],"blocking") && strcmp(argv[2],"event"))) {
        fprintf(stderr,"usage: %s schedule first|rr TRACE.csv\n       %s io blocking|event TRACE.csv\n",argv[0],argv[0]);
        return 2;
    }
    enum ut_policy policy = !strcmp(argv[2],"first") ? UT_FIRST_READY : UT_ROUND_ROBIN;
    pid_t child = -1;
    int pipefd[2] = {-1,-1};
    int is_io = !strcmp(argv[1],"io");
    event_mode = !strcmp(argv[2],"event");
    if (is_io) {
        if (pipe(pipefd) != 0) { perror("pipe"); return 1; }
        child = fork();
        if (child < 0) { perror("fork"); close(pipefd[0]); close(pipefd[1]); return 1; }
        if (!child) {
            close(pipefd[0]);
            struct timespec delay = {0, 200000000};
            while (nanosleep(&delay,&delay) != 0 && errno == EINTR) {}
            char c='X'; ssize_t n;
            do { n=write(pipefd[1],&c,1); } while (n<0 && errno==EINTR);
            close(pipefd[1]); _exit(n==1 ? 0 : 1);
        }
        close(pipefd[1]); read_fd=pipefd[0];
        if (event_mode && fcntl(read_fd,F_SETFL,O_NONBLOCK)<0) { perror("fcntl"); failed=1; }
    }
    if (!failed && ut_init(policy) != 0) { perror("ut_init"); failed=1; }
    if (!failed) {
        if (is_io) {
            if (ut_spawn(reader,NULL)<0 || ut_spawn(worker,NULL)<0) failed=1;
        } else {
            if (ut_spawn(steps,"A")<0 || ut_spawn(steps,"B")<0 || ut_spawn(steps,"C")<0) failed=1;
        }
        if (!failed && ut_run()!=0) { perror("ut_run"); failed=1; }
        if (ut_trace_write(argv[3])!=0) { perror("trace"); failed=1; }
        ut_destroy();
    }
    if (is_io) {
        close(read_fd);
        int status;
        pid_t waited;
        do { waited=waitpid(child,&status,0); } while(waited<0 && errno==EINTR);
        if (waited<0 || !WIFEXITED(status) || WEXITSTATUS(status)!=0) failed=1;
    }
    return failed ? 1 : 0;
}
