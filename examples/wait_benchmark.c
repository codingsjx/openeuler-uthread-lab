#define _POSIX_C_SOURCE 200809L
#include "uthread.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
static int pipes[31][2], readers, shared, yields, completed, failed;
static int ids[31];
static uint64_t checksum;
static int64_t micros(void) {
    struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))abort();
    return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000;
}
static long long cpu_us(const struct rusage *u) {
    return (long long)u->ru_utime.tv_sec*1000000+u->ru_utime.tv_usec
         +(long long)u->ru_stime.tv_sec*1000000+u->ru_stime.tv_usec;
}
static int number(const char *s,int max) {
    char *end;errno=0;long value=strtol(s,&end,10);
    if(errno||*end||value<1||value>max)exit(2);
    return (int)value;
}
static void reader(void *arg) {
    int id=*(int *)arg;char byte;
    if(ut_read(pipes[shared?0:id][0],&byte,1,10000)!=1||byte!='X')failed=1;
    else checksum+=(unsigned char)byte;
    completed++;
}
static void producer(void *arg) {
    (void)arg;
    for(int i=0;i<yields;i++){checksum++;ut_yield();}
    char data[31];memset(data,'X',sizeof data);
    int count=shared?1:readers;
    for(int i=0;i<count;i++) {
        size_t length=shared?(size_t)readers:1;
        ssize_t n;
        do{n=write(pipes[i][1],data,length);}while(n<0&&errno==EINTR);
        if(n!=(ssize_t)length)failed=1;
    }
    completed++;
}
int main(int argc,char **argv) {
    if(argc!=5||(strcmp(argv[2],"shared")&&strcmp(argv[2],"distinct"))||
       (strcmp(argv[4],"on")&&strcmp(argv[4],"off"))) {
        fprintf(stderr,"usage: %s READERS shared|distinct YIELDS on|off\n",argv[0]);return 2;
    }
    readers=number(argv[1],31);shared=!strcmp(argv[2],"shared");yields=number(argv[3],1000);
    int count=shared?1:readers;
    for(int i=0;i<count;i++) {
        if(pipe(pipes[i])||fcntl(pipes[i][0],F_SETFL,O_NONBLOCK)<0)return 1;
    }
    struct rusage before,after;if(getrusage(RUSAGE_SELF,&before))return 1;
    int64_t start=micros();
    if(ut_init_ex(UT_ROUND_ROBIN,!strcmp(argv[4],"off")?UT_TRACE_OFF:UT_TRACE_FULL))return 1;
    for(int i=0;i<readers;i++){ids[i]=i;if(ut_spawn(reader,&ids[i])<0)return 1;}
    if(ut_spawn(producer,NULL)<0||ut_run())return 1;
    int64_t wall=micros()-start;if(getrusage(RUSAGE_SELF,&after))return 1;
    struct ut_stats stats;if(ut_get_stats(&stats)||stats.dropped_events)return 1;
    if(failed||completed!=readers+1||checksum!=(uint64_t)yields+(uint64_t)readers*'X')return 1;
    printf("{\"readers\":%d,\"sharing\":\"%s\",\"yields\":%d,\"trace\":\"%s\","
           "\"wall_us\":%" PRId64 ",\"cpu_us\":%lld,\"completed\":%d,"
           "\"checksum\":\"%016" PRIx64 "\",\"poll_calls\":%llu,\"dispatches\":%llu}\n",
           readers,argv[2],yields,argv[4],wall,cpu_us(&after)-cpu_us(&before),completed,
           checksum,stats.poll_calls,stats.dispatches);
    ut_destroy();for(int i=0;i<count;i++){close(pipes[i][0]);close(pipes[i][1]);}
    return 0;
}
