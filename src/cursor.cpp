// Screen-space cursor: the target is projected to the screen, moved by mouse pixels and cast back
// onto the terrain. Only the movement the game's cursor update adds is replaced.
#include "common.h"
#include <math.h>

static int g_on, g_normal;
static float g_speed;
static bool g_bad;
volatile int g_curOver;
volatile float g_curDx, g_curDy;

// Pixels per mouse count of the Windows pointer, without acceleration.
static float WindowsSpeed() {
    static const float k[21] = { 1, 0.03125f, 0.0625f, 0.125f, 0.25f, 0.375f, 0.5f, 0.625f, 0.75f, 0.875f, 1,
                                 1.25f, 1.5f, 1.75f, 2, 2.25f, 2.5f, 2.75f, 3, 3.25f, 3.5f };
    int v = 10;
    SystemParametersInfoA(SPI_GETMOUSESPEED, 0, &v, 0);
    return k[v < 1 ? 1 : v > 20 ? 20 : v];
}
static float g_windows = 1;

struct V3 { float x, y, z; };

// CAM_View3Dto2D / CAM_View2Dto3D: x, y in 0..1, z = view depth.
static V3 Project(const V3& p) {
    V3 o = { 0, 0, 0 };
    ((void(__cdecl*)(int, const V3*, V3*))0x619440)(0, &p, &o);
    return o;
}
static V3 Unproject(float x, float y, float depth) {
    V3 in = { x, y, depth }, o = { 0, 0, 0 };
    ((void(__cdecl*)(int, const V3*, V3*))0x619410)(0, &in, &o);
    return o;
}
// Visible surface, water included.
static float Surface(V3 p) {
    return ((float(__cdecl*)(V3*))Dust(0x10144090))(&p);
}

// First point where the ray through (x, y) goes below the surface.
static bool Cast(float x, float y, V3& hit) {
    V3 a = Unproject(x, y, 1.0f), b = Unproject(x, y, 1001.0f);
    V3 d = { (b.x - a.x) / 1000, (b.y - a.y) / 1000, (b.z - a.z) / 1000 };
    float len = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    if (!(len > 0.001f && len < 1000.0f)) return false;
    float step = 1.5f / len;                         // one terrain cell
    bool above = false;
    float prev = 0;
    for (float t = 0; t < 2500.0f / len; t += step) {
        V3 p = { a.x + d.x * t, a.y + d.y * t, a.z + d.z * t };
        if (fabsf(p.x) > 333.5f || fabsf(p.y) > 333.5f) {
            if (above) return false;                 // left the map without a hit
            continue;
        }
        if (p.z > Surface(p)) { above = true; prev = t; continue; }
        if (!above) continue;                        // started inside a hill
        float lo = prev, hi = t;
        for (int i = 0; i < 14; i++) {
            float m = (lo + hi) / 2;
            V3 q = { a.x + d.x * m, a.y + d.y * m, a.z + d.z * m };
            if (q.z > Surface(q)) lo = m;
            else hi = m;
        }
        hit = { a.x + d.x * hi, a.y + d.y * hi, a.z + d.z * hi };
        return true;
    }
    return false;
}

static float Clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// Panning moves the target along with the camera, so on uneven ground the pointer would wander on
// screen. While panning, its place on screen is kept and the target follows.
static int g_hold, g_push;
volatile int g_panned;               // set by the pan code (mouse.cpp)
static bool g_anchor;                // the values below are from the previous frame
static float g_ax, g_ay;
// Without push-back the pointer has a place on screen that only the mouse changes. The target is
// cast from it when the mouse or the camera moves; where there is nothing to aim at, from the
// nearest ground below.
static float g_vx, g_vy, g_vsign = 1;
static DWORD g_vt;                   // last use
static bool g_vused;
static bool g_same;                  // the pan code uses the same coordinates
static bool Virtual() { return g_on && !g_push && g_normal; }
static bool Alive() { return g_vused && GetTickCount() - g_vt < 400; }
static void Used() { g_vt = GetTickCount(); g_vused = true; }
static bool CameraMoved() {
    static V3 eye, dir;
    V3 a = Unproject(0.5f, 0.5f, 1.0f), b = Unproject(0.5f, 0.5f, 101.0f), d = { b.x - a.x, b.y - a.y, b.z - a.z };
    bool m = fabsf(a.x - eye.x) + fabsf(a.y - eye.y) + fabsf(a.z - eye.z) > 0.03f ||
             fabsf(d.x - dir.x) + fabsf(d.y - dir.y) + fabsf(d.z - dir.z) > 0.05f;
    if (m) { eye = a; dir = d; }
    return m;
}
static bool Turning(uintptr_t ctl) {
    auto map = At<uintptr_t>(ctl), api = At<uintptr_t>(Dust(0x1019aec0));
    if (!map || !api) return false;
    if (At<float>(map + 0x2b8) != 0) return true;                          // mouse button
    auto down = At<int(__cdecl*)(int)>(api + 0x920);
    return down(At<int>(map + 3 * 0x18 + 0x208)) || down(At<int>(map + 2 * 0x18 + 0x208));
}
static void Aim(uintptr_t im, uintptr_t ctl, uintptr_t cur, bool input) {
    float mx = input ? At<float>(im + 0x138) : 0, my = input ? At<float>(im + 0x13c) : 0;
    RECT rc;
    if (!GetClientRect(At<HWND>(0x921500), &rc) || rc.right < 64 || rc.bottom < 64) return;
    V3 t = { At<float>(cur + 0x6d8), At<float>(cur + 0x6dc), 0 };
    t.z = Surface(t);
    bool moved = CameraMoved();
    // Which way is up on screen.
    V3 m0 = Unproject(0.5f, 0.5f, 100.0f), m1 = { m0.x, m0.y, m0.z + 1 };
    V3 q0 = Project(m0), q1 = Project(m1);
    if (fabsf(q1.y - q0.y) > 1e-4f) g_vsign = q1.y > q0.y ? 1.0f : -1.0f;
    if (!Alive()) {                                  // first use, or back from a cinematic
        V3 p = Project(t);
        if (!(p.z > 0.1f)) return;
        g_vx = Clamp(p.x, 0.004f, 0.996f);
        g_vy = Clamp(p.y, 0.004f, 0.996f);
        moved = true;
    }
    Used();
    // The camera turns around the target, which must not move then. Afterwards, once the camera has
    // come to rest or the mouse moves, aim from the pointer again.
    static bool turned;
    if (Turning(ctl)) { turned = true; return; }
    if (turned) {
        if (mx == 0 && my == 0 && moved) return;
        turned = false;
        moved = true;
    }
    if (mx == 0 && my == 0 && !moved) return;
    float s = Clamp((At<float>(ctl + 0x18) - 0.07f) * 4.34783f, 0, 1);
    float px = g_windows * powf(16.0f, s - 0.5f) * g_speed;   // slider middle = Windows pointer
    g_vx = Clamp(g_vx + mx * px / rc.right, 0.004f, 0.996f);
    g_vy = Clamp(g_vy - g_vsign * my * px / rc.bottom, 0.004f, 0.996f);
    float y = g_vy;
    for (int i = 0; i < 24; i++, y -= g_vsign * 0.03f) {
        if (y < 0.004f || y > 0.996f) break;
        V3 h;
        if (!Cast(g_vx, y, h)) continue;
        g_curDx = h.x - t.x;
        g_curDy = h.y - t.y;
        g_curOver = 1;
        return;
    }
    g_curDx = g_curDy = 0;
    g_curOver = 1;
}
static bool Panning(uintptr_t ctl) {
    static bool held;
    static V3 eye, dir;
    V3 a = Unproject(0.5f, 0.5f, 1.0f), b = Unproject(0.5f, 0.5f, 101.0f), d = { b.x - a.x, b.y - a.y, b.z - a.z };
    bool moved = fabsf(a.x - eye.x) + fabsf(a.y - eye.y) + fabsf(a.z - eye.z) > 0.002f;
    bool turned = fabsf(d.x - dir.x) + fabsf(d.y - dir.y) + fabsf(d.z - dir.z) > 0.05f;
    eye = a;
    dir = d;
    bool keys = false;
    auto api = At<uintptr_t>(Dust(0x1019aec0)), map = At<uintptr_t>(ctl);
    if (api && map) {
        auto down = At<int(__cdecl*)(int)>(api + 0x920);
        for (int i = 0; i < 4; i++) keys |= down(At<int>(map + i * 0x18 + 0x1a8)) != 0;
    }
    bool pan = g_panned != 0;
    g_panned = 0;
    if (keys ? g_hold : pan && !g_push) held = true;
    else if (keys || !moved || turned) held = false;         // the camera may glide on after release
    return held;
}

static void Compute(uintptr_t ui) {
    g_curOver = 0;
    bool anchored = g_anchor;
    g_anchor = false;
    auto g = Game();
    auto im = g ? At<uintptr_t>(g + 0x48) : 0, cur = At<uintptr_t>(Dust(0x1018d5d0));
    auto ctl = im ? At<uintptr_t>(im + 0x11c) : 0, st = At<uintptr_t>(ui + 0x154);
    if (!ctl || !cur || !st) return;
    // Keyboard and mouse, mouse movement on and the cursor free to move.
    if (!At<uint8_t>(ctl + 0x10) || !At<uint8_t>(ctl + 0x12) || !At<int>(ui + 0x2c)) return;
    bool input = At<int>(st + 0xd0) != 0;            // the mouse moves the cursor this frame
    if (Virtual()) { Aim(im, ctl, cur, input); return; }
    float mx = input ? At<float>(im + 0x138) : 0, my = input ? At<float>(im + 0x13c) : 0;   // raw counts
    bool may = g_hold || !g_push, hold = may && Panning(ctl);
    if (mx == 0 && my == 0 && !may) return;

    RECT rc;
    HWND w = At<HWND>(0x921500);
    if (!GetClientRect(w, &rc) || rc.right < 64 || rc.bottom < 64) return;
    V3 t = { At<float>(cur + 0x6d8), At<float>(cur + 0x6dc), 0 };
    t.z = Surface(t);
    V3 p = Project(t), up = Project({ t.x, t.y, t.z + 1 });
    if (!(p.z > 0.1f)) return;
    float sign = up.y > p.y ? 1.0f : -1.0f;
    if (anchored && hold) {
        p.x = g_ax;
        p.y = g_ay;
    }
    g_ax = p.x;
    g_ay = p.y;
    g_anchor = true;
    if (mx == 0 && my == 0 && !(hold && anchored)) return;

    float s = Clamp((At<float>(ctl + 0x18) - 0.07f) * 4.34783f, 0, 1);
    float px = g_windows * powf(16.0f, s - 0.5f) * g_speed;   // slider middle = Windows pointer
    float dx = mx * px / rc.right, dy = -sign * my * px / rc.bottom;
    // Above the horizon there is no ground: shorten the move until there is.
    for (int i = 0; i < 6; i++, dx /= 2, dy /= 2) {
        float x = Clamp(p.x + dx, 0.004f, 0.996f), y = Clamp(p.y + dy, 0.004f, 0.996f);
        V3 h;
        if (!Cast(x, y, h)) continue;
        g_curDx = h.x - t.x;
        g_curDy = h.y - t.y;
        g_ax = x;
        g_ay = y;
        g_curOver = 1;
        return;
    }
    if (mx == 0 && my == 0) return;
    g_curDx = g_curDy = 0;
    g_curOver = 1;
}

// ---- the worm --------------------------------------------------------------------
// KnowledgeCursor (cursor+0x6c4). The carried matter is a separate object.
typedef void(__fastcall* VisT)(uintptr_t, void*, int);
static VisT oVis;
static bool g_hide;
static void __fastcall hVis(uintptr_t self, void* edx, int visible) {
    oVis(self, edx, g_hide && PointerReady() ? 0 : visible);
}
static void HideWorm(uintptr_t cur, bool hide) {
    static uintptr_t last;
    static int n;
    auto worm = At<uintptr_t>(cur + 0x6c4);
    if (!oVis || !worm) return;
    if (hide != g_hide || worm != last || (hide && ++n % 30 == 0)) {
        g_hide = hide;
        last = worm;
        oVis(worm, nullptr, hide ? 0 : At<uint8_t>(cur + 0x7dc) & 1);
    }
}

// Hidden while the game hides its own cursor.
static void ShowPointer() {
    auto g = Game();
    auto im = g ? At<uintptr_t>(g + 0x48) : 0, cur = At<uintptr_t>(Dust(0x1018d5d0));
    auto ctl = im ? At<uintptr_t>(im + 0x11c) : 0;
    if (!ctl || !cur) return;
    bool show = At<uint8_t>(ctl + 0x10) && !At<int>(im + 0x34) && (At<uint8_t>(cur + 0x7dc) & 1);
    HideWorm(cur, show && PointerReady());
    if (!show) return;
    if (Virtual() && Alive()) {
        PointerShow(g_vx, g_vsign > 0 ? 1 - g_vy : g_vy);
        return;
    }
    V3 t = { At<float>(cur + 0x6d8), At<float>(cur + 0x6dc), 0 };
    t.z = Surface(t);
    V3 p = Project(t);
    V3 up = Project({ t.x, t.y, t.z + 1 });
    if (!(p.z > 0.1f)) return;
    PointerShow(p.x, up.y > p.y ? 1 - p.y : p.y);
}

typedef void(__fastcall* MoveT)(uintptr_t, void*);
static MoveT oMove;
static void __fastcall hMove(uintptr_t ui, void* edx) {
    g_curOver = 0;
    if (g_on && !g_bad) {
        __try {
            Compute(ui);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_curOver = 0;
            g_bad = true;
            Log("[cursor] screen-space cursor raised %08x, using ground movement", GetExceptionCode());
        }
    }
    oMove(ui, edx);
    g_curOver = 0;
    if (g_normal && !g_bad) {
        __try {
            ShowPointer();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_bad = true;
            Log("[cursor] pointer projection raised %08x", GetExceptionCode());
        }
    } else if (g_bad && g_hide) {
        __try {
            if (auto cur = At<uintptr_t>(Dust(0x1018d5d0))) HideWorm(cur, false);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_hide = false;
        }
    }
}

// Replaces: addss xmm6, xmm1 / addss xmm5, xmm2 (target += movement).
static __declspec(naked) void AddStub() {
    __asm {
        cmp g_curOver, 0
        je original
        movss xmm1, g_curDx
        movss xmm2, g_curDy
    original:
        addss xmm6, xmm1
        addss xmm5, xmm2
        ret
    }
}

void CursorInit() {
    g_on = IniInt("Mouse", "ScreenSpaceCursor", 1);
    g_speed = IniFloat("Mouse", "CursorSpeed", 1.0f);
    g_normal = IniInt("Mouse", "NormalCursor", 1);
    g_hold = IniInt("Mouse", "HoldPointerWhilePanning", 1);
    g_push = IniInt("Mouse", "EdgePushBack", 0);
    g_windows = WindowsSpeed();
    int epp[3] = {};
    SystemParametersInfoA(SPI_GETMOUSE, 0, epp, 0);
    Log("[cursor] Windows pointer speed %.3f, enhanced precision %s", g_windows, epp[2] ? "on" : "off");
}

void CursorDustLoaded() {
    g_bad = false;
    if (!g_normal && !IniInt("Mouse", "BreathIdle", 1))
        Patch("breath idle", Dust(0x10090363), "8d 41 44 50 e8", "eb 1e");
    if (!g_on && !g_normal) return;
    if (!BytesMatch(0x619410, "8b 44 24 04 8b 0d d4 c0 91 00 50 e8") || !BytesMatch(0x619440, "8b 44 24 04 8b 0d d4 c0 91 00 50 e8") ||
        !BytesMatch(Dust(0x10144090), "55 8b ec 83 e4 f0 83 ec 74") || !BytesMatch(Dust(0x10112a48), "f3 0f 58 f1 f3 0f 58 ea")) {
        Log("[cursor] code differs, skipped");
        return;
    }
    oMove = (MoveT)Hook("cursor update", Dust(0x10112460), "83 ec 4c a1 ?? ?? ?? ?? 53 8b 58 2c", 8, (void*)hMove);
    if (!oMove) return;
    g_hide = false;
    if (g_normal) oVis = (VisT)Hook("worm visibility", Dust(0x100909b0), "55 0f b6 6c 24 08 57 8b f9", 7, (void*)hVis);
    if (!g_on) return;
    uint8_t b[8] = { 0xE8, 0, 0, 0, 0, 0x90, 0x90, 0x90 };
    *(int32_t*)(b + 1) = (int32_t)((uintptr_t)AddStub - (Dust(0x10112a48) + 5));
    WriteMem(Dust(0x10112a48), b, 8);
    Log("[cursor] screen-space cursor ok");
}

// Gives the pan code the place of the pointer instead of the target's.
void __cdecl PanPointer(float* out) {
    if (!Virtual() || !Alive()) return;
    if (!g_same) {
        if (fabsf(out[0] - g_vx) < 0.05f && fabsf(out[1] - g_vy) < 0.05f && fabsf(g_vy - 0.5f) > 0.1f) g_same = true;
        else return;
    }
    out[0] = g_vx;
    out[1] = g_vy;
}
