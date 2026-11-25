// Start-up, configuration, hooking and dinput8 forwarding.
#include "common.h"
#include "version.h"
#include <ctype.h>
#include <stdarg.h>

Config cfg;
uintptr_t g_dust = 0;
char g_dir[MAX_PATH];
static char g_ini[MAX_PATH];
static FILE* g_log;

void Log(const char* fmt, ...) {
    if (!g_log) return;
    char line[1024];
    va_list a;
    va_start(a, fmt);
    _vsnprintf(line, sizeof line - 1, fmt, a);
    line[sizeof line - 1] = 0;
    va_end(a);
    fprintf(g_log, "%8u %s\n", GetTickCount(), line);   // one call, so that threads do not mix lines
    fflush(g_log);
}
bool Foreground() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}
int IniInt(const char* s, const char* k, int d) { return (int)GetPrivateProfileIntA(s, k, d, g_ini); }
float IniFloat(const char* s, const char* k, float d) {
    char b[64], def[64];
    _snprintf(def, 63, "%g", d);
    GetPrivateProfileStringA(s, k, def, b, 64, g_ini);
    return (float)atof(b);
}

// ---- patching -------------------------------------------------------------------
static bool Readable(uintptr_t va, size_t n) {
    MEMORY_BASIC_INFORMATION m;
    return VirtualQuery((void*)va, &m, sizeof m) && m.State == MEM_COMMIT &&
           !(m.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
           va + n <= (uintptr_t)m.BaseAddress + m.RegionSize;
}
bool BytesMatch(uintptr_t va, const char* p) {
    if (!Readable(va, 32)) return false;
    auto b = (const uint8_t*)va;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (*p == '?') { p += 2; b++; continue; }
        char h[3] = { p[0], p[1], 0 };
        if (*b++ != (uint8_t)strtoul(h, nullptr, 16)) return false;
        p += 2;
    }
    return true;
}
bool WriteMem(uintptr_t va, const void* src, size_t n) {
    DWORD o;
    if (!VirtualProtect((void*)va, n, PAGE_EXECUTE_READWRITE, &o)) return false;
    memcpy((void*)va, src, n);
    VirtualProtect((void*)va, n, o, &o);
    FlushInstructionCache(GetCurrentProcess(), (void*)va, n);
    return true;
}
bool Patch(const char* name, uintptr_t va, const char* expect, const char* bytes) {
    if (!BytesMatch(va, expect)) { Log("[patch] %s at %08x: bytes differ, skipped", name, (unsigned)va); return false; }
    uint8_t buf[64];
    size_t n = 0;
    for (const char* p = bytes; *p && n < sizeof buf;) {
        while (*p == ' ') p++;
        if (!*p) break;
        char h[3] = { p[0], p[1], 0 };
        buf[n++] = (uint8_t)strtoul(h, nullptr, 16);
        p += 2;
    }
    bool ok = WriteMem(va, buf, n);
    Log("[patch] %s at %08x %s", name, (unsigned)va, ok ? "ok" : "failed");
    return ok;
}
// Trampolines. Hooks in the gameplay DLL are installed again with every load and keep their slot.
static uint8_t* Slot(const char* name) {
    enum { COUNT = 64, SIZE = 64 };
    static uint8_t* mem;
    static const char* used[COUNT];
    if (!mem) mem = (uint8_t*)VirtualAlloc(nullptr, COUNT * SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    for (int i = 0; mem && i < COUNT; i++) {
        if (used[i] && strcmp(used[i], name)) continue;
        used[i] = name;
        return mem + i * SIZE;
    }
    return nullptr;
}
void* Hook(const char* name, uintptr_t va, const char* expect, int n, void* to) {
    if (n < 5 || !BytesMatch(va, expect)) { Log("[hook] %s at %08x: bytes differ, skipped", name, (unsigned)va); return nullptr; }
    auto t = Slot(name);
    if (!t) return nullptr;
    if (n > 32) return nullptr;
    memcpy(t, (void*)va, n);
    t[n] = 0xE9;
    *(int32_t*)(t + n + 1) = (int32_t)(va + n - (uintptr_t)(t + n + 5));
    uint8_t j[32];
    memset(j, 0x90, sizeof j);
    j[0] = 0xE9;
    *(int32_t*)(j + 1) = (int32_t)((uintptr_t)to - (va + 5));
    if (!WriteMem(va, j, n)) return nullptr;
    Log("[hook] %s at %08x ok", name, (unsigned)va);
    return t;
}
void* HookIAT(HMODULE mod, const char* dll, const char* fn, void* to) {
    auto b = (uint8_t*)mod;
    auto nt = (IMAGE_NT_HEADERS*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(b + nt->OptionalHeader.DataDirectory[1].VirtualAddress);
    for (; imp->Name; imp++) {
        if (_stricmp((char*)b + imp->Name, dll)) continue;
        auto names = (uintptr_t*)(b + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto iat = (uintptr_t*)(b + imp->FirstThunk);
        for (; *names; names++, iat++) {
            if (*names & 0x80000000) continue;
            if (strcmp((char*)((IMAGE_IMPORT_BY_NAME*)(b + *names))->Name, fn)) continue;
            void* old = (void*)*iat;
            WriteMem((uintptr_t)iat, &to, 4);
            Log("[iat] %s!%s ok", dll, fn);
            return old;
        }
    }
    Log("[iat] %s!%s not found", dll, fn);
    return nullptr;
}

// ---- offline start ----------------------------------------------------------------
// WinMain keeps the -offline switch in 921510 and whether the Documents folder has a local profile
// in 921511. Without a profile, starting offline could leave cloud saves behind, and the game asks.
// Replaces cmp byte ptr [921510], 1.
static int g_always;
static __declspec(naked) void OfflineStub() {
    __asm {
        cmp g_always, 0
        jne enable
        cmp byte ptr ds:[0x921511], 0
        je done
    enable:
        mov byte ptr ds:[0x921510], 1
    done:
        cmp byte ptr ds:[0x921510], 1
        ret
    }
}
static void ApplyOffline() {
    if (!cfg.offline) return;
    g_always = cfg.offline == 2;
    if (!BytesMatch(0x413473, "80 3d 10 15 92 00 01 75 68 80 3d 11 15 92 00 00")) { Log("[offline] code differs, skipped"); return; }
    uint8_t b[7] = { 0xE8, 0, 0, 0, 0, 0x90, 0x90 };
    *(int32_t*)(b + 1) = (int32_t)((uintptr_t)OfflineStub - (0x413473 + 5));
    WriteMem(0x413473, b, 7);
    Log("[offline] ok");
}

// ---- DLL load notification -------------------------------------------------------------
// The simulation's worker threads are created below normal priority.
static void WorkerPriority(uintptr_t kernel) {
    if (kernel) JobsInit(kernel);
    if (kernel && IniInt("General", "WorkerPriority", 1)) Patch("worker priority", kernel + 0x3e11, "6a ff 50 89 02 ff 15", "6a 00");
}
struct UStr { USHORT Len, Max; PWSTR Buf; };
struct LdrData { ULONG Flags; const UStr *Full, *Base; PVOID DllBase; ULONG Size; };
static VOID CALLBACK OnDll(ULONG reason, const LdrData* d, PVOID) {
    static const wchar_t kernel[] = L"GalileoKernel.dll";
    if (d->Base->Len == sizeof kernel - 2 && !_wcsnicmp(d->Base->Buf, kernel, d->Base->Len / 2)) {
        if (reason == 1) WorkerPriority((uintptr_t)d->DllBase);
        else JobsUnloaded();
        return;
    }
    static const wchar_t want[] = L"dustai.win32.f.dll";
    if (d->Base->Len != sizeof want - 2 || _wcsnicmp(d->Base->Buf, want, d->Base->Len / 2)) return;
    if (reason == 1) {
        auto h = (IMAGE_NT_HEADERS*)((BYTE*)d->DllBase + ((IMAGE_DOS_HEADER*)d->DllBase)->e_lfanew);
        if (h->FileHeader.TimeDateStamp != 0x4f4b63a8 || h->OptionalHeader.SizeOfImage != 0x1ba000) {
            Log("[dust] unknown gameplay DLL (%08x), left alone", h->FileHeader.TimeDateStamp);
            return;
        }
        g_dust = (uintptr_t)d->DllBase;
        Log("[dust] gameplay DLL at %08x", (unsigned)g_dust);
        CutscenesDustLoaded();
        MouseDustLoaded();
        CursorDustLoaded();
        FrameDustLoaded();
        RatesDustLoaded();
        QualityDustLoaded();
        EditorDustLoaded();
        SandboxDustLoaded();
        FreeplayDustLoaded();
    } else if (g_dust) {
        g_dust = 0;
        Log("[dust] gameplay DLL unloaded");
    }
}

// ---- message pump ----------------------------------------------------------------------
static decltype(&PeekMessageA) oPeek;
static BOOL WINAPI hPeek(LPMSG m, HWND w, UINT a, UINT b, UINT r) {
    g_beat = (g_beat + 1) | 1;
    BOOL got = oPeek(m, w, a, b, r);
    if (!got) {
        bool fg = Foreground();
        CutscenesFrame(fg);
        MouseFrame();
        EditorFrame(fg && !SandboxActive() && !FreeplayActive());
        SandboxFrame(fg);
        FreeplayFrame(fg);
    }
    return got;
}

// ---- configuration ----------------------------------------------------------------------
static int Key(const char* s) {
    static const struct { const char* name; int vk; } names[] = {
        { "Space", VK_SPACE }, { "Enter", VK_RETURN }, { "Escape", VK_ESCAPE }, { "Esc", VK_ESCAPE }, { "Tab", VK_TAB }, { "Backspace", VK_BACK },
        { "Shift", VK_SHIFT }, { "Ctrl", VK_CONTROL }, { "Alt", VK_MENU }, { "Insert", VK_INSERT }, { "Delete", VK_DELETE }, { "Home", VK_HOME },
        { "End", VK_END }, { "PageUp", VK_PRIOR }, { "PageDown", VK_NEXT }, { "Up", VK_UP }, { "Down", VK_DOWN }, { "Left", VK_LEFT },
        { "Right", VK_RIGHT }, { "Pause", VK_PAUSE }, { "ScrollLock", VK_SCROLL }, { "MouseMiddle", VK_MBUTTON }, { "Mouse4", VK_XBUTTON1 }, { "Mouse5", VK_XBUTTON2 },
    };
    for (auto& k : names) if (!_stricmp(s, k.name)) return k.vk;
    if (s[0] && !s[1] && isalnum((unsigned char)s[0])) return toupper((unsigned char)s[0]);
    int v = atoi(s + 1);
    if (toupper((unsigned char)s[0]) == 'F' && v >= 1 && v <= 24) return VK_F1 + v - 1;
    if (!_strnicmp(s, "Numpad", 6) && s[6] >= '0' && s[6] <= '9' && !s[7]) return VK_NUMPAD0 + s[6] - '0';
    return 0;
}
int IniKeys(const char* section, const char* key, const char* def, int* out, int max) {
    char b[256];
    GetPrivateProfileStringA(section, key, def, b, sizeof b, g_ini);
    int n = 0;
    for (char* s = strtok(b, ", \t"); s && n < max; s = strtok(nullptr, ", \t")) {
        if (int k = Key(s)) out[n++] = k;
        else if (_stricmp(s, "None")) Log("[ini] %s: unknown key \"%s\"", key, s);
    }
    return n;
}
static void LoadConfig() {
    cfg.offline = IniInt("General", "Offline", 1);
    cfg.debug = IniInt("General", "Diagnostics", 0);
    cfg.skipVideos = IniInt("Cutscenes", "SkipVideos", 1);
    cfg.skipCinematics = IniInt("Cutscenes", "SkipCinematics", 1);
    cfg.autoSkipCinematics = IniInt("Cutscenes", "AutoSkipCinematics", 0);
    IniKeys("Cutscenes", "SkipKeys", "Space, Enter", cfg.skipKeys, 8);
}

// ---- performance cores ------------------------------------------------------------------
// Every simulation step waits for its slowest worker, and Windows 10 may schedule workers on
// efficiency cores. Keep the process on the performance cores of hybrid CPUs.
static void UsePerformanceCores() {
    if (!IniInt("General", "PerformanceCores", 1)) return;
    auto get = (BOOL(WINAPI*)(LOGICAL_PROCESSOR_RELATIONSHIP, PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, PDWORD))GetProcAddress(
        GetModuleHandleA("kernel32.dll"), "GetLogicalProcessorInformationEx");
    DWORD n = 0;
    if (!get) return;
    get(RelationProcessorCore, nullptr, &n);
    auto buf = (BYTE*)malloc(n ? n : 1);
    if (!buf || !get(RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &n)) { free(buf); return; }
    int best = -1, worst = 256;
    DWORD_PTR fast = 0, all = 0;
    int groups = 0, cores = 0;
    for (DWORD o = 0; o < n;) {
        auto p = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + o);
        int c = p->Processor.EfficiencyClass;
        auto m = p->Processor.GroupMask[0].Mask;
        if (p->Processor.GroupMask[0].Group) groups = 1;
        all |= m;
        if (c > best) { best = c; fast = 0; cores = 0; }
        if (c == best) { fast |= m; cores++; }
        if (c < worst) worst = c;
        o += p->Size;
    }
    free(buf);
    if (groups || best <= worst || cores < 4) { Log("[cores] left alone"); return; }
    DWORD_PTR have = 0, sys = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &have, &sys);
    BOOL ok = (have & fast) && SetProcessAffinityMask(GetCurrentProcess(), have & fast);
    Log("[cores] performance cores %x of %x: %s", (unsigned)fast, (unsigned)all, ok ? "set" : "failed");
}

// ---- dinput8 forwarding ------------------------------------------------------------------
static HMODULE g_real;
static FARPROC Real(const char* n) {
    if (!g_real) {
        char p[MAX_PATH];
        GetSystemDirectoryA(p, MAX_PATH);
        strcat(p, "\\dinput8.dll");
        g_real = LoadLibraryA(p);
    }
    return g_real ? GetProcAddress(g_real, n) : nullptr;
}
extern "C" HRESULT WINAPI P_DirectInput8Create(HINSTANCE a, DWORD b, REFIID c, LPVOID* d, LPVOID e) {
    auto f = (HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPVOID))Real("DirectInput8Create");
    return f ? f(a, b, c, d, e) : E_FAIL;
}
extern "C" HRESULT WINAPI P_DllCanUnloadNow() { return S_FALSE; }
extern "C" HRESULT WINAPI P_DllGetClassObject(REFCLSID a, REFIID b, LPVOID* c) {
    auto f = (HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*))Real("DllGetClassObject");
    return f ? f(a, b, c) : E_FAIL;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(h);
    GetModuleFileNameA(h, g_dir, MAX_PATH);
    if (char* s = strrchr(g_dir, '\\')) s[1] = 0;
    _snprintf(g_ini, MAX_PATH - 1, "%sFromDust.SkoFix.ini", g_dir);
    if (IniInt("General", "Log", 1)) {
        char p[MAX_PATH];
        _snprintf(p, MAX_PATH - 1, "%sFromDust.SkoFix.log", g_dir);
        p[MAX_PATH - 1] = 0;
        g_log = fopen(p, "w");
    }
    Log("From Dust SkoFix " VERSION);
    if (GetFileAttributesA(g_ini) == INVALID_FILE_ATTRIBUTES) Log("[ini] %s not found, using defaults", g_ini);
    LoadConfig();
    HMODULE exe = GetModuleHandleA(nullptr);
    // WinMain's "-offline" test identifies the supported build.
    if ((uintptr_t)exe != 0x400000 || !BytesMatch(0x413405, "6a 07 68 60 cd 89 00")) {
        Log("[init] unknown executable, staying passive");
        return TRUE;
    }
    UsePerformanceCores();
    WorkerPriority((uintptr_t)GetModuleHandleA("GalileoKernel.dll"));
    ApplyOffline();
    DisplayInit();
    CutscenesInit();
    MouseInit();
    CursorInit();
    PointerInit();
    EditorInit();
    SandboxInit();
    FreeplayInit();
    FrameInit();
    RatesInit();
    QualityInit();
    WaterInit();
    HangInit();
    oPeek = (decltype(oPeek))HookIAT(exe, "USER32.dll", "PeekMessageA", (void*)hPeek);
    auto reg = (LONG(NTAPI*)(ULONG, PVOID, PVOID, PVOID*))GetProcAddress(GetModuleHandleA("ntdll.dll"), "LdrRegisterDllNotification");
    static PVOID cookie;
    if (reg) reg(0, (PVOID)OnDll, nullptr, &cookie);
    return TRUE;
}
