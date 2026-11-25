// Frame pacing, and the water and terrain simulation at 30 steps per second.
//
// The simulation advances one step per kick, without a time step. A frame without a kick also
// skips the join. During play the kick is moved from the beginning of the frame to its end and
// joined after the pacing wait, so that the step runs in the idle time; nothing touches the
// simulation in between. Such a step comes after the job system's end of frame and needs its own
// begin (kernel+b8) and end (kernel+c0), see jobs.cpp.
//
// Fluid emission (dustai 10067350) pours a fixed amount per call and follows the simulation.
// Everything else that depends on the frame rate is in rates.cpp.
#include "common.h"
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

static int g_fps;
static float g_noCap = 1e9f;
static double g_freq;
static unsigned g_frames, g_kicks;
static double g_work, g_worst, g_game;
bool g_simFrame = true;              // this frame advances the simulation
static int g_defer = 1;
static void* g_pending;              // GalileoInterface to kick at the end of the frame
static bool InPlay();
typedef void(__fastcall* KickT)(void*, void*, int);
static KickT oKick;
typedef void(__fastcall* KernelT)(uintptr_t, void*, int);
static KernelT Kernel(uintptr_t k, int slot) { return At<KernelT>(At<uintptr_t>(k) + slot); }
typedef void(__fastcall* EmitT)(void*, void*);
static EmitT oEmit;

bool HighRate() { return g_fps > 30; }

static double Now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart / g_freq;
}

// [General] Diagnostics=1: F10 hides the pointer, F11 switches between 30 and 60 fps, and the log
// gets a summary every ten seconds.
static void Diagnostics(double now) {
    static bool was10, was11;
    static double report;
    bool fg = Foreground(), k10 = fg && (GetAsyncKeyState(VK_F10) & 0x8000), k11 = fg && (GetAsyncKeyState(VK_F11) & 0x8000);
    if (k10 && !was10) PointerToggle();
    if (k11 && !was11 && (g_fps > 30 || oEmit)) {
        g_fps = g_fps > 30 ? 30 : 60;
        Log("[frame] switched to %d fps", g_fps);
    }
    was10 = k10;
    was11 = k11;
    if (now < report + 10) return;
    if (report && g_frames) {
        char jobs[100];
        JobStats(jobs, sizeof jobs);
        double s = now - report;
        Log("[frame] %.1f fps, %.1f simulation steps/s, game time x%.2f, work %.1f ms avg %.1f worst, %s",
            g_frames / s, g_kicks / s, g_game / s, g_work * 1000 / g_frames, g_worst * 1000, jobs);
    }
    g_frames = g_kicks = 0;
    g_work = g_worst = g_game = 0;
    report = now;
}

typedef void(__fastcall* FrameT)(void*, void*);
static FrameT oFrame;
static void __fastcall hFrame(void* self, void* edx) {
    static double next;
    g_beat = (g_beat + 1) | 1;
    const double step = 1.0 / g_fps;
    double now = Now();
    if (cfg.debug) Diagnostics(now);
    if (now > next + 0.25) next = now;          // start, or a long hitch
    next += step;
    if (next < now) next = now;
    static double sim;
    const double simStep = 1.0 / 30.0;
    g_simFrame = g_fps <= 30 || now >= sim - 0.002;
    if (g_simFrame) sim = now > sim + simStep ? now + simStep : sim + simStep;   // no backlog
    g_frames++;
    MousePreFrame();
    RatesPreFrame();
    double t0 = Now();
    g_doing = "frame";
    oFrame(self, edx);
    g_doing = "after frame";
    double w = Now() - t0;
    g_work += w;
    if (w > g_worst) g_worst = w;
    g_game += At<float>(0x92975c);
    // Start the simulation step, unless the level has ended.
    void* galileo = nullptr;
    if (g_pending) {
        if (InPlay() && At<void*>(0x921534) == g_pending) {
            galileo = g_pending;
            g_doing = "simulation start";
            auto kernel = At<uintptr_t>((uintptr_t)galileo + 4);
            Kernel(kernel, 0xb8)(kernel, nullptr, 0);             // begin frame
            oKick(galileo, nullptr, 1);
            g_doing = "pacing";
        }
        g_pending = nullptr;
    }
    while ((now = Now()) < next) {
        if (next - now > 0.002) Sleep(1);
        else YieldProcessor();
    }
    g_doing = "simulation wait";
    if (galileo) {
        ((void(__fastcall*)(void*, void*))0x5d9660)((char*)galileo + 0x18, nullptr);      // its frame listener
        auto kernel = At<uintptr_t>((uintptr_t)galileo + 4);
        Kernel(kernel, 0xc0)(kernel, nullptr, At<int>(0x938590));   // end frame
    }
    g_doing = "between frames";
}

static void __fastcall hKick(void* self, void* edx, int on) {
    if (!on) { oKick(self, edx, on); return; }
    if (!g_simFrame) return;
    g_kicks++;
    if (g_defer && InPlay()) { g_pending = self; return; }
    oKick(self, edx, on);
}

static bool InPlay() {
    auto g = Game();
    auto f = g ? At<uintptr_t>(g + 0x74) : 0;
    return f && At<int>(f + 4) == 7;
}
static void __fastcall hEmit(void* self, void* edx) {
    if (g_simFrame) oEmit(self, edx);
}
void FrameDustLoaded() {
    if (g_fps == 0 || !oKick) return;
    oEmit = (EmitT)Hook("fluid emission", Dust(0x10067350), "83 ec 1c f3 0f 10 05 ?? ?? ?? ?? 55", 11, (void*)hEmit);
    if (!oEmit && g_fps > 30) { g_fps = 30; Log("[frame] emission hook failed, staying at 30 fps"); }
}

void FrameInit() {
    g_fps = IniInt("Frame", "TargetFps", 60);
    if (g_fps == 0) return;
    if (g_fps < 30) g_fps = 30;
    if (g_fps > 120) g_fps = 120;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freq = (double)f.QuadPart;
    if (!BytesMatch(0x5c19d8, "f3 0f 2a c8 f3 0f 5e c8 0f 2f 0d 04 31 8a 00 77 5a") ||
        !BytesMatch(0x5d9620, "8b 44 24 04 85 c0 0f 95 c2 88 51 1c") || !BytesMatch(0x455d40, "0f 87 16 02 00 00") ||
        !BytesMatch(0x5d9660, "56 8b f1 80 7e 05 00") || !BytesMatch(0x5c1960, "51 55 56 57 8b f1 e8")) {
        Log("[frame] code differs, left alone");
        return;
    }
    oFrame = (FrameT)Hook("frame", 0x5c1960, "51 55 56 57 8b f1 e8", 6, (void*)hFrame);
    if (!oFrame) return;
    g_defer = IniInt("Frame", "DeferSimulation", 1);
    oKick = (KickT)Hook("sim kick", 0x5d9620, "8b 44 24 04 85 c0", 6, (void*)hKick);
    if (oKick) Patch("sim step limiter", 0x455d40, "0f 87 16 02 00 00", "90 90 90 90 90 90");
    else if (g_fps > 30) g_fps = 30;            // the simulation must stay at 30 steps
    uintptr_t p = (uintptr_t)&g_noCap;
    WriteMem(0x5c19e3, &p, 4);
    timeBeginPeriod(1);
    if (g_fps > 30) {                           // an effect that adds 1/30 per frame
        uintptr_t dt = 0x92975c;
        if (BytesMatch(0x474652, "f3 0f 58 05 d8 13 8a 00")) WriteMem(0x474656, &dt, 4);
    }
    Log("[frame] target %d fps, deferred simulation %s", g_fps, g_defer ? "on" : "off");
}
