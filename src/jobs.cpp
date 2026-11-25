// GalileoKernel's begin frame (+27d0) empties the job queue without waiting or locking. A worker
// that wakes to an empty queue quits for good, so a job still running at that moment can cost
// workers until the simulation never finishes. Empty the queue only when idle, under its lock.
//   kernel+a14 manager: +e20 lock, +ebc workers that have quit, +2360 queue
//   queue: +8 first unfinished job, +c jobs submitted, +98 jobs running
#include "common.h"

typedef void(__fastcall* BeginT)(uintptr_t, void*, int);
static BeginT oBegin;
static uintptr_t g_mgr;
static unsigned g_begins, g_busy, g_forced;
static double g_waitMax;

static void __fastcall hBegin(uintptr_t k, void* edx, int arg) {
    auto mgr = At<uintptr_t>(k + 0xa14);
    auto q = mgr ? At<uintptr_t>(mgr + 0x2360) : 0;
    g_begins++;
    if (!q) { oBegin(k, edx, arg); return; }
    g_mgr = mgr;
    auto lock = (CRITICAL_SECTION*)(mgr + 0xe20);
    DWORD t0 = GetTickCount();
    bool waited = false;
    for (;;) {
        EnterCriticalSection(lock);
        bool idle = At<int>(q + 0xc) == At<int>(q + 8) && At<int>(q + 0x98) == 0;
        DWORD t = GetTickCount() - t0;
        if (idle || t > 3000) {
            if (!idle) { g_forced++; Log("[jobs] queue still busy after 3 s, emptied anyway"); }
            oBegin(k, edx, arg);
            LeaveCriticalSection(lock);
            if (t > g_waitMax) g_waitMax = t;
            return;
        }
        LeaveCriticalSection(lock);
        if (!waited) { waited = true; g_busy++; }
        SwitchToThread();
    }
}
void JobsInit(uintptr_t kernel) {
    if (oBegin) return;
    oBegin = (BeginT)Hook("job queue reset", kernel + 0x27d0, "55 8b ec 83 e4 f0 83 ec 0c 56 8d 44 24 08 8b f1 ff 86 40 0a 00 00", 6, (void*)hBegin);
}
void JobsUnloaded() {
    oBegin = nullptr;
    g_mgr = 0;
}
void JobStats(char* out, int size) {
    _snprintf(out, size - 1, "job queue: %u resets, %u found busy (%.0f ms worst), workers lost %d", g_begins, g_busy, g_waitMax,
              g_mgr ? At<int>(g_mgr + 0xebc) : -1);
    out[size - 1] = 0;
    g_begins = g_busy = 0;
    g_waitMax = 0;
}
