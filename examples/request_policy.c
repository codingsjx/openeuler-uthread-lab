#include "request_policy.h"
const char *request_phase_name(enum request_phase phase) {
    switch (phase) {
        case REQUEST_HEADER: return "header";
        case REQUEST_BODY: return "body";
        case REQUEST_WRITE: return "write";
        case REQUEST_IDLE: return "idle";
    }
    return "unknown";
}
int request_reclaimable(enum request_admission policy, const struct request_progress *progress,
                        int64_t now, int stall_ms) {
    if (!progress || stall_ms < 1 || progress->phase == REQUEST_IDLE) return 0;
    if (policy == ADMISSION_BYTE_STALLED)
        return now - progress->last_progress_ms >= stall_ms &&
               (progress->phase == REQUEST_WRITE || !progress->received_bytes);
    if (policy == ADMISSION_PHASE_AWARE)
        return now - progress->stage_started_ms >= stall_ms &&
               (progress->phase == REQUEST_HEADER || progress->phase == REQUEST_BODY ||
                progress->phase == REQUEST_WRITE);
    return 0;
}
