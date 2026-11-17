#ifndef REQUEST_POLICY_H
#define REQUEST_POLICY_H
#include <stdint.h>
enum request_phase { REQUEST_HEADER, REQUEST_BODY, REQUEST_WRITE, REQUEST_IDLE };
enum request_admission { ADMISSION_REJECT, ADMISSION_BYTE_STALLED, ADMISSION_PHASE_AWARE };
struct request_progress {
    enum request_phase phase;
    int64_t stage_started_ms, last_progress_ms;
    unsigned long long received_bytes;
};
const char *request_phase_name(enum request_phase phase);
/* Pure policy decision. Caller must also check ownership, actual I/O wait,
 * kernel readiness and cancellation state before reclaiming a connection. */
int request_reclaimable(enum request_admission policy, const struct request_progress *progress,
                        int64_t now_ms, int stall_ms);
#endif
