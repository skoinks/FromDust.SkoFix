// Freeze watchdog: writes a minidump when the main thread has neither drawn a frame nor pumped
// messages for eight seconds.
#include "common.h"
#include <dbghelp.h>

volatile LONG g_beat;                 // incremented by the frame and message pump hooks
const char* volatile g_doing = "";    // what the frame hook is doing

typedef BOOL(WINAPI* WriteT)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

static DWORD WINAPI Watch(LPVOID) {
    HMODULE h = LoadLibraryA("dbghelp.dll");
    auto write = h ? (WriteT)GetProcAddress(h, "MiniDumpWriteDump") : nullptr;
    LONG last = 0;
    DWORD since = GetTickCount();
    bool done = false;
    int n = 0;
    for (;;) {
        Sleep(500);
        LONG b = g_beat;
        DWORD now = GetTickCount();
        if (b != last || !b) {
            if (done) Log("[hang] running again after %u ms", now - since);
            last = b;
            since = now;
            done = false;
            continue;
        }
        if (done || now - since < 8000) continue;
        done = true;
        Log("[hang] no frame for %u ms, last in: %s", now - since, g_doing);
        if (!write || n++ >= 3) continue;
        char p[MAX_PATH];
        _snprintf(p, MAX_PATH - 1, "%sFromDust.SkoFix-hang%d.dmp", g_dir, n);
        p[MAX_PATH - 1] = 0;
        HANDLE f = CreateFileA(p, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), f,
                        (MINIDUMP_TYPE)(MiniDumpWithDataSegs | MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory), nullptr, nullptr, nullptr);
        CloseHandle(f);
        Log("[hang] dump %d %s", n, ok ? "written" : "failed");
    }
}
void HangInit() {
    if (!IniInt("General", "HangDump", 0)) return;
    HANDLE t = CreateThread(nullptr, 0, Watch, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}
