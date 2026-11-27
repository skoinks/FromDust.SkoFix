// Direct mouse response. Removes what the game does to the mouse counts: scaling by frame time, a
// minimum step, rotation on one axis at a time, replay of old counts, the pull towards objects,
// slowing on other ground, a push at the screen edges and one frame of delay.
#include "common.h"
#include <math.h>

static struct {
    int enabled, rawCamera, flatTerrain, snap, edgeFix, lag;
    float cursor, camera;
} m;

static const float kStep = 1.0f / 30.0f;   // original speed at 30 fps

static float CleanDt(float dt) { return dt > 0.0005f ? (dt < 0.1667f ? dt : 0.1667f) : kStep; }

// ---- stale deltas ------------------------------------------------------------
// The device object owns the poll's state at +2c and replaces it when the window changes.
static uintptr_t g_owner;
static uintptr_t Wrapper() { return g_owner ? At<uintptr_t>(g_owner + 0x2c) : 0; }
typedef int(__fastcall* OwnerT)(uintptr_t, void*, int);
static OwnerT oOwner;
static int __fastcall hOwner(uintptr_t self, void* edx, int a) {
    g_owner = self;
    return oOwner(self, edx, a);
}
static DWORD g_polled;
static bool g_fresh, g_freshOk;    // polled this frame
typedef char(__fastcall* PollT)(uintptr_t, void*);
static PollT oPoll;
static char __fastcall hPoll(uintptr_t self, void* edx) {
    g_polled = GetTickCount();
    if (g_fresh && self == Wrapper()) { g_fresh = false; return g_freshOk; }
    char ok = oPoll(self, edx);
    if (!ok) At<int>(self + 0xC) = At<int>(self + 0x10) = At<int>(self + 0x14) = 0;
    return ok;
}
// Polls before the frame; the game's own poll later in the frame reuses the result.
void MousePreFrame() {
    auto w = Wrapper();
    if (!m.enabled || !oPoll || !w || GetTickCount() - g_polled > 200 || GetFocus() != At<HWND>(w + 0x20)) { g_fresh = false; return; }
    int x = 0, y = 0, z = 0;
    if (g_fresh) { x = At<int>(w + 0xC); y = At<int>(w + 0x10); z = At<int>(w + 0x14); }   // not consumed
    g_freshOk = oPoll(w, nullptr) != 0;
    if (!g_freshOk) At<int>(w + 0xC) = At<int>(w + 0x10) = At<int>(w + 0x14) = 0;
    At<int>(w + 0xC) += x;
    At<int>(w + 0x10) += y;
    At<int>(w + 0x14) += z;
    g_fresh = true;
}
// ---- camera pan speed ----------------------------------------------------------------
// The pan code scales the camera and target movement by a constant (50). Its two loads read
// g_panScale instead, which is larger while the key is held.
static int g_fastKey;
static float g_fast = 2.0f, g_panBase;
static float g_panScale;
void MouseFrame() {
    if (g_panBase) g_panScale = g_fastKey && Foreground() && (GetAsyncKeyState(g_fastKey) & 0x8000) ? g_panBase * g_fast : g_panBase;
    auto w = m.enabled && oPoll ? Wrapper() : 0;
    if (w && GetTickCount() - g_polled > 200) At<int>(w + 0xC) = At<int>(w + 0x10) = At<int>(w + 0x14) = 0;
}

// ---- cursor speed --------------------------------------------------------------
// Replaces S = clamp((sensitivity - 0.07) * 4.34783, 0, 1) in the cursor update, which computes
// speed = norm * (0.04 * S + 0.03) and multiplies by dt. Returns an S that cancels dt.
static float __cdecl CursorS(uintptr_t controls, float dt) {
    float s = (At<float>(controls + 0x18) - 0.07f) * 4.34783f;
    s = s < 0 ? 0 : s > 1 ? 1 : s;
    if (!At<uint8_t>(controls + 0x12)) return s;   // mouse movement off
    // The ends of the slider are 1/4 and 4 times its middle.
    float speed = 0.0125f * powf(16.0f, s - 0.5f);
    return (kStep / CleanDt(dt) * m.cursor * speed - 0.03f) / 0.04f;
}
static __declspec(naked) void CursorStub() {
    __asm {
        mov eax, [esp + 0x18]   // caller's dt
        push eax
        push ecx
        call CursorS            // result in st(0), as the original
        add esp, 8
        ret
    }
}

// ---- edge scrolling ----------------------------------------------------------------
// The cursor follows at 1 - (x - 1) * 20 camera speeds, 2x at the start of the edge zone (0.95).
// Limit it to 1x.
static const float kZero = 0.0f, kTwenty = 20.0f, kFour = 4.0f, kFifteen = 15.0f;
static __declspec(naked) void EdgeRight() {
    __asm {
        maxss xmm0, kZero
        movaps xmm2, xmm5
        subss xmm2, xmm0
        ret
    }
}
static __declspec(naked) void EdgeLeft() {
    __asm {
        mulss xmm3, kTwenty
        minss xmm3, kZero
        ret
    }
}
static __declspec(naked) void EdgeTop() {
    __asm {
        mulss xmm4, kFour
        maxss xmm4, kZero
        ret
    }
}
static __declspec(naked) void EdgeBottom() {
    __asm {
        mulss xmm3, kFifteen
        minss xmm3, kZero
        ret
    }
}
static void CallAt(const char* name, uintptr_t va, const char* expect, void* stub, int n) {
    if (!BytesMatch(va, expect)) { Log("[mouse] %s differs, skipped", name); return; }
    uint8_t b[16];
    memset(b, 0x90, sizeof b);
    b[0] = 0xE8;
    *(int32_t*)(b + 1) = (int32_t)((uintptr_t)stub - (va + 5));
    WriteMem(va, b, n);
    Log("[mouse] %s ok", name);
}

// ---- camera ----------------------------------------------------------------------
typedef void(__fastcall* CamT)(uintptr_t, void*);
static CamT oCam;
static void __fastcall hCam(uintptr_t im, void* edx) {
    oCam(im, edx);
    float dt = CleanDt(At<float>(0x92975c));
    At<float>(im + 0x130) *= kStep / dt * m.camera;   // multiplied by dt again
    At<float>(im + 0x134) *= 0.327f * m.camera;       // applied twice; matches yaw
}

// ---- zoom ------------------------------------------------------------------------------
// The wheel sets a zoom of 0..1 that blends the camera distance from the near view to the far one.
// The camera modes copy their settings from the camera manager; the far distance of mode 1 is at
// +c4 of the copy, the near one at +3c.
static float g_zoomOut = 1.5f;
typedef void(__fastcall* SettingsT)(uintptr_t, void*, int, uintptr_t);
static SettingsT oSettings;
static void __fastcall hSettings(uintptr_t cm, void* edx, int mode, uintptr_t out) {
    oSettings(cm, edx, mode, out);
    if (mode != 1 || !out) return;
    static bool logged;
    float& dist = At<float>(out + 0xc4);
    if (!logged) Log("[mouse] zoom from %.1f to %.1f, now %.1f", At<float>(out + 0x3c), dist, dist * g_zoomOut);
    logged = true;
    dist *= g_zoomOut;
}

void MouseInit() {
    m.enabled = IniInt("Mouse", "DirectResponse", 1);
    m.rawCamera = IniInt("Mouse", "DirectCamera", 1);
    m.flatTerrain = IniInt("Mouse", "IgnoreTerrainSlope", 1);
    m.snap = IniInt("Mouse", "SnapToObjects", 0);
    m.edgeFix = IniInt("Mouse", "SmoothEdgeScroll", 1);
    m.lag = IniInt("Mouse", "BreathLag", 1);
    m.cursor = IniFloat("Mouse", "CursorSpeed", 1.0f);
    m.camera = IniFloat("Mouse", "CameraSpeed", 1.0f);
    int key = 0;
    if (IniKeys("Mouse", "FastPanKey", "Ctrl", &key, 1)) g_fastKey = key;
    g_fast = IniFloat("Mouse", "FastPanSpeed", 2.0f);
    g_zoomOut = IniFloat("Mouse", "ZoomOut", 1.5f);
    if (m.enabled) oOwner = (OwnerT)Hook("mouse device", 0x809b80, "55 56 8b f1 33 ed 39 6e 20", 6, (void*)hOwner);
    if (oOwner) oPoll = (PollT)Hook("mouse poll", 0x8048d0, "53 56 8b f1 32 db 83 7e 08 00", 6, (void*)hPoll);
}

// The pan code moves the camera this frame. Replaces mov ecx, [game].
static uintptr_t g_gamePtr;
static __declspec(naked) void PanStub() {
    __asm {
        mov g_panned, 1
        mov ecx, g_gamePtr
        mov ecx, [ecx]
        ret
    }
}
// The pan code projects the cursor to test the edges. Replaces call edx; mov eax, [esi+4], with
// the arguments (0, position, result) on the stack.
static uintptr_t g_panRet;
static __declspec(naked) void PanProjectStub() {
    __asm {
        pop g_panRet
        call edx
        push dword ptr [esp + 8]
        call PanPointer
        add esp, 4
        mov eax, [esi + 4]
        jmp g_panRet
    }
}
bool g_rotateInPlace;
void MouseDustLoaded() {
    if (!IniInt("Mouse", "EdgePushBack", 0) && IniInt("Mouse", "ScreenSpaceCursor", 1)) {
        // Keyboard panning keeps the cursor in 7..93% of the width, 7..90% of the height and within
        // 50 units of the camera's target. Skip these corrections.
        static const struct { uintptr_t va; const char *from, *to; } push[] = {
            { 0x1011e25c, "0f 86 8a 00 00 00 f3 0f 5c e3", "90 e9 8a 00 00 00" },
            { 0x1011e305, "72 71 f3 0f 5c de", "eb 71" },
            { 0x1011e389, "0f 86 80 00 00 00 f3 0f 5c de", "90 e9 80 00 00 00" },
            { 0x1011e4b8, "0f 82 8e 00 00 00 f3 0f 10 5c 24 50", "90 e9 8e 00 00 00" },
            { 0x1011e557, "72 70 f3 0f 5c de", "eb 70" },
        };
        for (auto& p : push) Patch("pan push-back", Dust(p.va), p.from, p.to);
        g_gamePtr = Dust(0x1017a89c);
        CallAt("pan position", Dust(0x1011db67), "ff d2 8b 46 04 8b 48 0c 8b 11 8b 42 10 83 c4 0c", (void*)PanProjectStub, 5);
        CallAt("pan flag", Dust(0x1011e5d1), "8b 0d ?? ?? ?? ?? 8b 59 44 8b cb e8", (void*)PanStub, 6);
    }
    // While the camera rotates its centre glides to the cursor. Take the branch of frames that do not.
    g_rotateInPlace = !IniInt("Mouse", "RecenterWhileRotating", 0) &&
        Patch("rotation recentering", Dust(0x1011e7b3), "0f 84 c0 00 00 00 f3 0f 10 7e 28", "90 e9 c0 00 00 00");
    // Frames that pan neither the camera nor the cursor test the cursor against the playable area
    // and glide the camera to it when it is outside. Skip the test.
    if (!IniInt("Mouse", "FollowPointerOffField", 0))
        Patch("off-field follow", Dust(0x1011e879), "83 7c 24 1c 00 0f 84 a4 fd ff ff", "83 7c 24 1c 00 90 e9 a4 fd ff ff");
    if (g_fastKey && g_fast > 0 && g_fast != 1) {
        static const uintptr_t loads[] = { 0x1011e639, 0x1011e6be };
        uintptr_t k = Dust(0x1015a968);
        bool ok = BytesMatch(Dust(0x1011e639), "f3 0f 10 3d ?? ?? ?? ?? f3 0f 58 dd") &&
                  BytesMatch(Dust(0x1011e6be), "f3 0f 10 2d ?? ?? ?? ?? f3 0f 59 c5");
        for (auto va : loads) ok = ok && At<uintptr_t>(Dust(va) + 4) == k;
        if (ok) {
            g_panBase = g_panScale = At<float>(k);
            uintptr_t p = (uintptr_t)&g_panScale;
            for (auto va : loads) WriteMem(Dust(va) + 4, &p, 4);
        }
        Log("[mouse] fast pan %s", ok ? "ok" : "differs, skipped");
    }
    if (g_zoomOut > 0 && g_zoomOut != 1)
        oSettings = (SettingsT)Hook("camera settings", Dust(0x10119740), "8b 44 24 04 48 83 f8 03 77 38 ff 24 85", 8, (void*)hSettings);
    // Always take the branch for frames that pan neither the camera nor the cursor.
    if (!IniInt("Mouse", "EdgePan", 1)) Patch("edge pan off", Dust(0x1011ddbf), "0f 85 ec 07 00 00 f3 0f 10 5c 24 60", "90 e9 ec 07 00 00");
    if (!m.enabled) return;
    uintptr_t call = Dust(0x10112714);
    if (BytesMatch(call, "e8 d7 0a 01 00 d8 4c 24 18 d8 44 24 1c")) {
        int32_t rel = (int32_t)((uintptr_t)CursorStub - (call + 5));
        WriteMem(call + 1, &rel, 4);
        Log("[mouse] cursor response ok");
    } else Log("[mouse] cursor call differs, skipped");
    Patch("cursor minimum step", Dust(0x101128f3), "76 48 f3 0f 51 e0", "eb");
    if (m.flatTerrain) {
        uint8_t p[8] = { 0xf3, 0x0f, 0x10, 0x05 };
        *(uint32_t*)(p + 4) = (uint32_t)Dust(0x10155524);   // 1.0f
        bool ok = BytesMatch(Dust(0x1011587e), "f3 0f 10 87 2c 18 00 00") && WriteMem(Dust(0x1011587e), p, 8);
        Log("[mouse] terrain drag %s", ok ? "off" : "differs, skipped");
    }
    if (!m.snap) {   // detection stays, prompts use it
        Patch("snap pull", Dust(0x10114176), "e8 15 e2 ff ff f3 0f 10 86 70 01 00 00", "83 c4 10 90 90");
        Patch("snap hold", Dust(0x101141a8), "74 33 a1", "eb");
    }
    if (!m.lag && !IniInt("Mouse", "NormalCursor", 1)) Patch("breath lag", Dust(0x1009041f), "72 03 0f 28 fa 0f 2f fa", "90 90");
    if (m.edgeFix) {
        CallAt("edge right", Dust(0x1011de22), "0f 28 d5 f3 0f 5c d0", (void*)EdgeRight, 7);
        CallAt("edge left", Dust(0x1011de8a), "f3 0f 59 1d", (void*)EdgeLeft, 8);
        CallAt("edge bottom", Dust(0x1011e10e), "f3 0f 59 1d ?? ?? ?? ?? f3 0f 5c e6", (void*)EdgeBottom, 8);
        // No ease-in: (1 - v) * dt * 4 becomes 1 - v.
        Patch("edge ease-in", Dust(0x1011e163), "f3 0f 59 35 ?? ?? ?? ?? f3 0f 58 f3", "0f 28 f5 f3 0f 5c f3 90");
        CallAt("edge top", Dust(0x1011df47), "f3 0f 59 25", (void*)EdgeTop, 8);
    }
    if (m.rawCamera) {
        Patch("camera axis snap", Dust(0x101177a4), "76 26 f3 0f 10 1d", "eb");
        oCam = (CamT)Hook("camera mouse", Dust(0x10117680), "83 ec 30 f3 0f 10 05 ?? ?? ?? ?? 56", 11, (void*)hCam);
    }
}
