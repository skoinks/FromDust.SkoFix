// Frame rate corrections. Everything runs every frame; what the game does per frame instead of per
// second is corrected where it happens:
//   Rate()      1 at 30 fps, 0.5 at 60: factor for an amount added every frame
//   g_simFrame  30 times a second: for what cannot be scaled
// Gameplay writes to the simulation are queued and executed every frame (dustai 100dce60), mostly
// as absolute values, so the queue and what fills it have to run at the same rate.
#include "common.h"
#include <math.h>

static int g_on;
extern bool g_simFrame;
bool HighRate();                    // frame.cpp: running above 30 fps

static float Rate() {
    if (!HighRate()) return 1;
    float k = At<float>(0x92975c) * 30.0f;
    return k > 0.01f && k < 1 ? k : 1;
}
static bool Held() { return HighRate() && !g_simFrame; }

// ---- amounts ---------------------------------------------------------------------------
typedef void(__fastcall* F2)(uintptr_t, void*, float, float);
static F2 oCaldeira, oCrack;
static void __fastcall hCaldeira(uintptr_t t, void* e, float dt, float scale) { oCaldeira(t, e, dt, scale * Rate()); }
static void __fastcall hCrack(uintptr_t t, void* e, float dt, float scale) { if (!Held()) oCrack(t, e, dt, scale); }

typedef void(__fastcall* F4)(uintptr_t, void*, void*, float, float, float);
static F4 oFiguier;                 // water tree: limit per cell and frame
static void __fastcall hFiguier(uintptr_t t, void* e, void* pos, float a, float b, float cap) { oFiguier(t, e, pos, a, b, cap * Rate()); }

typedef void(__fastcall* F0)(uintptr_t, void*);
static F0 oTrail, oErode, oErodeScan, oVillage;
static void __fastcall hTrail(uintptr_t t, void* e) { if (!Held()) oTrail(t, e); }
static void __fastcall hErode(uintptr_t t, void* e) { if (!Held()) oErode(t, e); }
static void __fastcall hErodeScan(uintptr_t t, void* e) { if (!Held()) oErodeScan(t, e); }
static void __fastcall hVillage(uintptr_t t, void* e) { if (!Held()) oVillage(t, e); }

// Terrain smoothing: height += (average - height) * weight
typedef void(__fastcall* FS)(uintptr_t, void*, void*, int, float);
static FS oSmoothA, oSmoothB;
static float Weight(float w) {
    float k = Rate();
    return k >= 1 || !(w > 0 && w < 1) ? w : 1 - powf(1 - w, k);
}
static void __fastcall hSmoothA(uintptr_t t, void* e, void* pos, int r, float w) { oSmoothA(t, e, pos, r, Weight(w)); }
static void __fastcall hSmoothB(uintptr_t t, void* e, void* pos, int r, float w) { oSmoothB(t, e, pos, r, Weight(w)); }

// Brush that blends a velocity into cells.
typedef void(__fastcall* FB)(uintptr_t, void*, float, float, float, int);
static FB oPush;
static void __fastcall hPush(uintptr_t t, void* e, float r, float s, float a, int neg) { if (!Held()) oPush(t, e, r, s, a, neg); }

// Animals: limit of the angle turned per frame.
typedef uintptr_t(__cdecl* FT)(void*, void*, void*, void*, float, float);
static FT oTurn;
static uintptr_t __cdecl hTurn(void* a, void* b, void* c, void* d, float pitch, float yaw) {
    float k = Rate();

    return oTurn(a, b, c, d, pitch * k, yaw * k);
}

// ---- constants applied once per frame, read from private copies ------------------------------
static float c_tenth = 0.1f, c_half = 0.5f, c_decay = 0.9f, c_human = 100.0f;
static bool Operand(const char* name, uintptr_t va, const char* expect, int at, float* to, float value) {
    if (!BytesMatch(va, expect)) { Log("[rates] %s differs, skipped", name); return false; }
    auto from = At<uintptr_t>(va + at);
    if (IsBadReadPtr((void*)from, 4) || At<float>(from) != value) { Log("[rates] %s: constant differs, skipped", name); return false; }
    return WriteMem(va + at, &to, 4);
}
// Before each frame, with the last frame's time.
void RatesPreFrame() {
    float k = Rate();
    c_tenth = 0.1f * k;
    c_half = 0.5f * k;
    c_decay = k >= 1 ? 0.9f : powf(0.9f, k);
    c_human = 100.0f / k;                        // multiplied by the frame time twice
}

// ---- engine ------------------------------------------------------------------------------
// Auto exposure: lum += (new - lum) * k, every frame.
static F0 oExposure;
static void __fastcall hExposure(uintptr_t t, void* e) {
    auto p = At<uintptr_t>(0x9229fc);
    float r = Rate();
    if (!p || r >= 1) { oExposure(t, e); return; }
    float k = At<float>(p + 0x88);
    At<float>(p + 0x88) = Weight(k);
    oExposure(t, e);
    At<float>(p + 0x88) = k;
}
// Maps that are fed back into themselves every frame: water flow (453b10), ground and common
// parameters (454e00), grass colour (454140). One pass per simulation step, decided in 455a40.
static bool g_pass = true;
static F0 oPrepare, oParams, oGrass;
static bool Play() {
    auto g = Game();
    auto f = g ? At<uintptr_t>(g + 0x74) : 0;
    return f && At<int>(f + 4) == 7;
}
static void __fastcall hPrepare(uintptr_t t, void* e) {
    static unsigned last, owed;
    auto gi = At<uintptr_t>(0x921534);
    auto sim = gi ? At<uintptr_t>(gi + 0xc) : 0;
    unsigned step = sim ? At<unsigned>(sim + 0x92c) : 0;
    owed += step - last;
    last = step;
    if (owed > 2) owed = 2;
    g_pass = !HighRate() || !sim || !Play() || owed;
    if (owed) owed--;
    oPrepare(t, e);
    g_pass = true;
}
static void __fastcall hParams(uintptr_t t, void* e) {
    bool ready = At<uintptr_t>(t + 0x1b8) && At<uintptr_t>(t + 0x1bc) && At<uintptr_t>(t + 0x1c0) && At<int>(0x921c34) >= 0xf;
    if (g_pass || !ready) oParams(t, e);
}
static void __fastcall hGrass(uintptr_t t, void* e) {
    if (g_pass || !At<uintptr_t>(t + 0x2f98)) oGrass(t, e);
}
// +1d8 two maps, +1e0 the one to write, +3054/+3058 0 = restart half the map.
typedef void(__fastcall* ScrollT)(uintptr_t, void*, int, int);
static ScrollT oScroll;
static void __fastcall hScroll(uintptr_t self, void* edx, int tex, int frozen) {
    static bool resetA, resetB;
    bool must = (At<BYTE>(self + 0x30b8) & 2) || At<int>(self + 0x3154) > 0 || !At<uintptr_t>(self + 0x1d8) || !At<uintptr_t>(self + 0x1dc);
    if (!g_pass && !must && !frozen) {
        resetA |= At<float>(self + 0x3054) == 0;
        resetB |= At<float>(self + 0x3058) == 0;
        At<BYTE>(self + 0x1e0) ^= 1;             // keep the map written last
        return;
    }
    if (!frozen) {
        if (resetA) At<float>(self + 0x3054) = 0;
        if (resetB) At<float>(self + 0x3058) = 0;
        resetA = resetB = false;
    }
    oScroll(self, edx, tex, frozen);
}

// Shimmer on flat water: a looping normal map of 8 images, 16 a second.
static float c_shimmer = 16.0f;
static void ShimmerInit() {
    float s = IniFloat("Graphics", "WaterShimmerSpeed", 0.5f);
    if (!(s >= 0 && s <= 4) || s == 1) return;
    const char *a = "f3 0f 59 05 74 ff 89 00", *b = "f3 0f 59 0d 74 ff 89 00";
    if (!BytesMatch(0x453e68, a) || !BytesMatch(0x4549e0, b)) { Log("[water] shimmer code differs, skipped"); return; }
    c_shimmer = 16.0f * s;
    Operand("shimmer map", 0x453e68, a, 4, &c_shimmer, 16.0f);
    Operand("shimmer surface", 0x4549e0, b, 4, &c_shimmer, 16.0f);
    Log("[water] shimmer speed x%.2f", s);
}

void RatesInit() {
    ShimmerInit();
    g_on = IniInt("Frame", "RateFixes", 1);
    oPrepare = (F0)Hook("terrain prepare", 0x455a40, "83 ec 08 53 8b 1d c0 b4 92 00", 10, (void*)hPrepare);
    if (oPrepare) oScroll = (ScrollT)Hook("water scroll", 0x453b10, "83 ec 18 a1 c0 b4 92 00 53", 8, (void*)hScroll);
    if (!g_on) return;
    if (oPrepare) {
        oParams = (F0)Hook("ground fade", 0x454e00, "83 ec 48 53 55 8b 2d c0 b4 92 00", 5, (void*)hParams);
        oGrass = (F0)Hook("grass fade", 0x454140, "55 8b ec 83 e4 f0 83 ec 44", 6, (void*)hGrass);
    }
    oExposure = (F0)Hook("exposure", 0x46d1e0, "83 ec 30 53 57 8b f9", 5, (void*)hExposure);
}
void RatesDustLoaded() {
    if (!g_on) return;
    oCaldeira = (F2)Hook("caldeira", Dust(0x10058b90), "81 ec 94 00 00 00 56 8b f1", 6, (void*)hCaldeira);
    oCrack = (F2)Hook("crack", Dust(0x10060160), "83 ec 34 56 8b f1 d9 86 d8 01", 6, (void*)hCrack);
    oFiguier = (F4)Hook("water tree", Dust(0x100834f0), "d9 44 24 10 8b 44 24 04 83 ec 10", 8, (void*)hFiguier);
    oTrail = (F0)Hook("creature trail", Dust(0x10021c50), "55 8b ec 83 e4 f8 83 ec 30 53", 6, (void*)hTrail);
    oErode = (F0)Hook("coast erosion", Dust(0x10107f50), "83 ec 20 53 55 56 57 8b e9 68", 5, (void*)hErode);
    oErodeScan = (F0)Hook("coast scan", Dust(0x101074c0), "55 8b ec 83 e4 f0 81 ec 94 06", 6, (void*)hErodeScan);
    oVillage = (F0)Hook("village ring", Dust(0x10077810), "55 8b ec 83 e4 f8 81 ec a0 00 00 00", 6, (void*)hVillage);
    oSmoothA = (FS)Hook("smoothing 1", Dust(0x1004e0e0), "83 ec 10 8b 44 24 14 8b 10", 7, (void*)hSmoothA);
    oSmoothB = (FS)Hook("smoothing 2", Dust(0x1004e160), "83 ec 10 8b 44 24 14 8b 10", 7, (void*)hSmoothB);
    oPush = (FB)Hook("velocity brush", Dust(0x10142010), "8b 41 04 83 ec 24 56 8b 31", 6, (void*)hPush);
    oTurn = (FT)Hook("animal turn", Dust(0x100d7700), "83 ec 34 56 f3 0f 10 0d", 12, (void*)hTurn);
    int n = 0;
    n += Operand("tool trigger ramp", Dust(0x101173b5), "f3 0f 10 25", 4, &c_tenth, 0.1f);
    n += Operand("keyboard pitch", Dust(0x10117be6), "f3 0f 59 0d", 4, &c_tenth, 0.1f);
    n += Operand("absorb per cell", Dust(0x100d9882), "f3 0f 10 0d", 4, &c_tenth, 0.1f);
    n += Operand("pour per cell", Dust(0x100da323), "f3 0f 10 05", 4, &c_half, 0.5f);
    n += Operand("sphere decay", Dust(0x10094505), "f3 0f 59 15", 4, &c_decay, 0.9f);
    n += Operand("human acceleration", Dust(0x100cf37b), "f3 0f 59 05", 4, &c_human, 100.0f);
    Log("[rates] %d of 6 constants replaced", n);
}
