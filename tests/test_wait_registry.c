#define _POSIX_C_SOURCE 200809L
#include "uthread.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>
#ifdef NDEBUG
#error Tests require assertions
#endif
static int pipes[2][2], finished, expired, max_fds, two_ready, repolled;
static int sequence[96], sequence_count, ids[32];
int __real_poll(struct pollfd *, nfds_t, int);
int __real___poll_chk(struct pollfd *, nfds_t, int, size_t);
/* Inspect the actual system-call arguments, then use real kernel I/O. */
int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout) {
    if ((int)count > max_fds) max_fds = (int)count;
    if (two_ready && finished < 2) repolled++;
    int result = __real_poll(fds,count,timeout);
    if (result == 2) two_ready = 1;
    return result;
}
/* Ubuntu's fortified libc redirects poll to __poll_chk. Preserve that check. */
int __wrap___poll_chk(struct pollfd *fds, nfds_t count, int timeout, size_t bytes) {
    if ((int)count > max_fds) max_fds = (int)count;
    if (two_ready && finished < 2) repolled++;
    int result = __real___poll_chk(fds,count,timeout,bytes);
    if (result == 2) two_ready = 1;
    return result;
}
static void prepare(int n) {
    finished=expired=max_fds=two_ready=repolled=0;
    for (int i=0;i<n;i++) {
        assert(pipe(pipes[i])==0);
        assert(fcntl(pipes[i][0],F_SETFL,O_NONBLOCK)==0);
    }
    assert(ut_init_ex(UT_ROUND_ROBIN,UT_TRACE_OFF)==0);
}
static void cleanup(int n) {
    ut_destroy();
    for(int i=0;i<n;i++) {close(pipes[i][0]);close(pipes[i][1]);}
}
static void short_reader(void *arg) {
    (void)arg;char byte;
    assert(ut_read(pipes[0][0],&byte,1,5)==-1 && errno==ETIMEDOUT);
    expired=1;finished++;
}
static void long_reader(void *arg) {
    (void)arg;char byte;
    assert(ut_read(pipes[0][0],&byte,1,3000)==1 && byte=='X');
    finished++;
}
static void delayed_writer(void *arg) {
    (void)arg;
    while(!expired)ut_yield();
    assert(write(pipes[0][1],"X",1)==1);
}
static void separate_reader(void *arg) {
    int id=*(int *)arg;char byte;
    assert(ut_read(pipes[id][0],&byte,1,3000)==1 && byte=='X');finished++;
}
static void two_writes(void *arg) {
    (void)arg;
    assert(write(pipes[0][1],"X",1)==1);
    assert(write(pipes[1][1],"X",1)==1);
}
static void invalid_reader(void *arg) {
    (void)arg;char byte;
    assert(ut_read(pipes[0][0],&byte,1,3000)==-1 && errno==EBADF);finished++;
}
static void close_read_end(void *arg) {(void)arg;assert(close(pipes[0][0])==0);}
static void ordered_task(void *arg) {
    int id=*(int *)arg;
    for(int i=0;i<3;i++) {sequence[sequence_count++]=id;ut_yield();}
}
static void empty_task(void *arg) {(void)arg;finished++;}
int main(void) {
    prepare(1);
    assert(ut_spawn(short_reader,NULL)>=0 && ut_spawn(long_reader,NULL)>=0);
    assert(ut_spawn(delayed_writer,NULL)>=0 && ut_run()==0);
    assert(finished==2 && max_fds==1);
    cleanup(1);
    prepare(2);
    int a=0,b=1;
    assert(ut_spawn(separate_reader,&a)>=0 && ut_spawn(separate_reader,&b)>=0);
    assert(ut_spawn(two_writes,NULL)>=0 && ut_run()==0);
    assert(finished==2 && max_fds==2 && two_ready && repolled==0);
    cleanup(2);
    prepare(1);
    assert(ut_spawn(invalid_reader,NULL)>=0 && ut_spawn(close_read_end,NULL)>=0);
    assert(ut_run()==0 && finished==1 && max_fds==1);
    close(pipes[0][1]);ut_destroy();
    assert(ut_init_ex(UT_ROUND_ROBIN,UT_TRACE_OFF)==0);
    for(int i=0;i<32;i++){ids[i]=i;assert(ut_spawn(ordered_task,&ids[i])==i);}
    assert(ut_run()==0 && sequence_count==96);
    for(int i=0;i<96;i++)assert(sequence[i]==i%32);
    finished=0;assert(ut_spawn(empty_task,NULL)==0);
    assert(ut_run()==0 && finished==1);ut_destroy();
    puts("PASS: shared-fd deadline isolation, compacted ready groups, POLLNVAL, bit 31 wrap, reused slots");
    return 0;
}
