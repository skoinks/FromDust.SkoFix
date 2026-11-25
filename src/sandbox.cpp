// The developers' world editor is part of the in-game HUD, which is told "sandbox allowed" = 0
// (dustai 100b726d). When allowed, gamepad Select toggles it. The HUD only listens to a gamepad,
// so keys are sent as its buttons.
#include "common.h"

static int g_on, g_toggle;
static bool g_dead;

void SandboxInit() {
    g_on = IniInt("Sandbox", "DevSandbox", 1);
    IniKeys("Sandbox", "ToggleKey", "F2", &g_toggle, 1);
}
void SandboxDustLoaded() {
    g_dead = false;
    if (g_on) g_on = Patch("sandbox allowed", Dust(0x100b726d), "6a 00 68 ?? ?? ?? ?? 6a 07 8d 4c 24 30", "6a 01") ||
                     BytesMatch(Dust(0x100b726d), "6a 01 68");
}

static int Mode() {                              // level game mode, 0 when not in a level
    auto g = Game();
    auto lp = g ? At<uintptr_t>(g + 0x18) : 0;
    return lp ? At<int>(lp + 0x94) : 0;
}
bool SandboxActive() {
    if (!g_on || g_dead) return false;
    int m = Mode();
    return m == 1 || m == 3 || m == 4;
}

static bool Send(int mask, bool down) {
    auto g = Game();
    auto holder = At<uintptr_t>(g + 0x2b8);
    if (!holder || !BytesMatch(Dust(0x100b5860), "83 ec 18 8b 44 24 1c 56") || !BytesMatch(Dust(0x100b9f90), "83 ec 08 53 55 56 57 8d 71 20")) return false;
    __try {
        auto ui = ((uintptr_t(__thiscall*)(uintptr_t, int))Dust(0x100b9f90))(holder, 0);
        if (!ui) return false;
        ((void(__thiscall*)(uintptr_t, int, int))Dust(down ? 0x100b5860 : 0x100b5800))(ui, At<int>(g + 0x14), mask);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_dead = true;
        Log("[sandbox] button %x raised %08x, sandbox keys off", mask, GetExceptionCode());
        return false;
    }
}

static const struct { int vk, mask; } kPad[] = {
    { VK_NUMPAD5, 0x1 }, { VK_NUMPAD0, 0x2 }, { VK_DECIMAL, 0x4 }, { VK_ADD, 0x8 },            // A B X Y
    { VK_NUMPAD7, 0x10 }, { VK_NUMPAD9, 0x20 }, { VK_MULTIPLY, 0x200 },                        // LB RB Start
    { VK_NUMPAD8, 0x1000 }, { VK_NUMPAD6, 0x2000 }, { VK_NUMPAD2, 0x4000 }, { VK_NUMPAD4, 0x8000 },
};
enum { N = sizeof kPad / sizeof *kPad };

void SandboxFrame(bool fg) {
    static bool was[N], wasToggle;
    static int lastMode = -1;
    if (!g_on || g_dead || !Game()) return;
    int m = Mode();
    if (m != lastMode) { Log("[sandbox] game mode %d", m); lastMode = m; }

    bool t = fg && g_toggle && (GetAsyncKeyState(g_toggle) & 0x8000);
    if (t && !wasToggle) {
        auto g = Game();
        auto flow = At<uintptr_t>(g + 0x74);
        bool ready = flow && At<int>(flow + 4) == 7 && (m == 1 || m == 2);
        static const int need[] = { 0x38, 0x3c, 0x40, 0x44, 0x48, 0x4c, 0x58 };
        for (int off : need) if (!At<uintptr_t>(g + off)) ready = false;
        if (ready && !At<uintptr_t>(At<uintptr_t>(g + 0x40) + 0xC)) ready = false;   // editor entity missing
        Log("[sandbox] toggle, mode %d%s", m, ready ? "" : " (not available here)");
        if (ready && Send(0x100, true)) Send(0x100, false);
    }
    wasToggle = t;

    bool active = SandboxActive();
    for (int i = 0; i < N; i++) {
        bool down = fg && active && (GetAsyncKeyState(kPad[i].vk) & 0x8000);
        if (down != was[i]) Send(kPad[i].mask, down);
        was[i] = down;
    }
}
