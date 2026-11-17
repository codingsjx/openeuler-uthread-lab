#include "request_policy.h"
#include <assert.h>
#include <stdio.h>
#ifdef NDEBUG
#error Tests require assertions
#endif
int main(void) {
    struct request_progress p = {REQUEST_HEADER, 100, 290, 1};
    assert(!request_reclaimable(ADMISSION_REJECT, &p, 300, 100));
    assert(!request_reclaimable(ADMISSION_BYTE_STALLED, &p, 300, 100));
    assert(request_reclaimable(ADMISSION_PHASE_AWARE, &p, 300, 100));
    p.phase = REQUEST_BODY;
    assert(request_reclaimable(ADMISSION_PHASE_AWARE, &p, 300, 100));
    assert(!request_reclaimable(ADMISSION_PHASE_AWARE, &p, 199, 100));
    p.phase = REQUEST_IDLE;
    assert(!request_reclaimable(ADMISSION_PHASE_AWARE, &p, 10000, 100));
    p.phase = REQUEST_WRITE;
    assert(!request_reclaimable(ADMISSION_BYTE_STALLED, &p, 300, 100));
    assert(request_reclaimable(ADMISSION_PHASE_AWARE, &p, 300, 100));
    assert(request_reclaimable(ADMISSION_BYTE_STALLED, &p, 400, 100));
    puts("PASS: byte progress versus fixed request-phase policy and idle protection");
    return 0;
}
