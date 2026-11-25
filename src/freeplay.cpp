// Sandbox. The challenge menu lists the story maps instead, and starts them like challenges, so that
// story progress and saves are not involved. Rules, villagers and their
// landmarks are off, all powers and natural events on.
#include "common.h"
#include <math.h>
#include <stdarg.h>

static int g_on;
static bool g_list;                  // the challenge menu shows the story maps
static bool g_active;                // a sandbox map is running
static bool g_playing, g_ready;
static int g_event = -1;
static bool g_panel = true;

static int State() {
    auto g = Game();
    auto f = g ? At<uintptr_t>(g + 0x74) : 0;
    return f ? At<int>(f + 4) : 0;
}

static bool Position(float* pos) {                  // of the pointer, in the world
    auto cursor = At<uintptr_t>(Dust(0x1018d5d0)), object = cursor ? At<uintptr_t>(cursor + 0xC) : 0;
    if (!object) return false;
    memcpy(pos, ((float*(__thiscall*)(uintptr_t))At<uintptr_t>(At<uintptr_t>(object) + 0xC))(object), 12);
    return true;
}

// ---- powers ------------------------------------------------------------------------------------
// The game has four slots at manager+4dc, 18 bytes each: power (-1 none), statue, time, duration,
// number, flags (1 ready, 2 active, 4 shown). Manager+62c bit 1: powers do not run out. Twelve keys
// share them: a key puts its power in a free slot and switches it on when the game has made it ready.
// Tap or hold the fifth and fourth action (Shift, X) to use powers 14 to 17.
static const struct { int vk; const char* key; int power; const char* label; } kPowers[] = {
    { '1', "1", 14, "Sand" }, { '2', "2", 15, "Lava" }, { '3', "3", 16, "Water" }, { '4', "4", 17, "Plants" },
    { '5', "5", 0, "Jellify water" }, { '6', "6", 7, "Evaporate water" }, { '7', "7", 8, "Slow fire" }, { '8', "8", 1, "Bigger sphere" },
    { '9', "9", 2, "Infinite earth" }, { '0', "0", 4, "Black hole" }, { VK_OEM_MINUS, "-", 3, "Caldera" }, { VK_OEM_PLUS, "=", 6, "Inverse caldera" },
};
enum { POWERS = sizeof kPowers / sizeof *kPowers };
static const struct { int power; const char* key[4]; const char* text[4]; } kUses[] = {   // of the active power
    { 14, { "Shift", "hold Shift", "X", "hold X" }, { "small mountain", "big mountain", "small caldera", "big caldera" } },
    { 15, { "Shift", "hold Shift", "X", "hold X" }, { "lava source", "volcano", "remove volcano or source", "crater" } },
    { 16, { "hold Shift", "X", "hold X" }, { "remove water source", "water source", "tsunami" } },
    { 17, { "hold Shift", "X", "hold X" }, { "remove tree", "tree", "add plants" } },
};
static int g_power = -1;             // key pressed
static int g_pending = -1, g_wait;   // slot to switch on, updates left
static uintptr_t Slot(uintptr_t spm, int i) { return spm + 0x4dc + i * 0x18; }
static void Fill(uintptr_t spm, int slot, int power) {
    auto s = Slot(spm, slot);
    At<int>(s) = power;
    At<float>(s + 8) = ((float(__thiscall*)(uintptr_t, uintptr_t))Dust(0x10127b50))(spm, s);
    At<float>(s + 0xc) = 0;
    At<unsigned>(s + 0x14) &= ~7u;
}
// Listener of the power keys, [game+38]+20: its held requests are at +18, one for each slot. Its
// 100149e0 switches a slot on, or off if it has a request.
static uintptr_t Listener() {
    auto input = At<uintptr_t>(Game() + 0x38);
    return input && At<uintptr_t>(input + 0x20) == Dust(0x10155858) ? input + 0x20 : 0;
}
typedef void(__fastcall* SwitchT)(uintptr_t, void*, int, void*, float*);
static SwitchT oSwitch;
static void __fastcall hSwitch(uintptr_t self, void* edx, int slot, void* unused, float* pos) {
    if (!g_active) oSwitch(self, edx, slot, unused, pos);          // the game's keys 1 to 4 do nothing here
}
static void Switch(int slot) {
    auto l = Listener();
    float pos[3];
    if (!l || !Position(pos)) { Log("[freeplay] slot %d cannot be switched", slot + 1); return; }
    __try {
        oSwitch(l, nullptr, slot, nullptr, pos);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[freeplay] switching slot %d raised %08x", slot + 1, GetExceptionCode());
    }
}
static void Press(uintptr_t spm, int key) {
    int power = kPowers[key].power, free = -1;
    auto l = Listener();
    g_pending = -1;
    for (int i = 3; i >= 0; i--) {
        auto s = Slot(spm, i);
        if (At<int>(s) == power) {
            Log("[freeplay] key %s: %s, slot %d, flags %x", kPowers[key].key, kPowers[key].label, i + 1, At<unsigned>(s + 0x14));
            if (At<unsigned>(s + 0x14) & 3) Switch(i);
            return;
        }
        if (!(At<unsigned>(s + 0x14) & 2) && l && !At<uintptr_t>(l + 0x18 + i * 4)) free = i;
    }
    Log("[freeplay] key %s: %s, to slot %d", kPowers[key].key, kPowers[key].label, free + 1);
    if (free < 0) return;
    Fill(spm, free, power);
    g_pending = free;
    g_wait = 30;
}
// The manager may outlive the map: its limits are put back on the way out.
static uintptr_t g_kept;
static unsigned g_limits[19];
static bool g_cheat;
static void Restore() {
    auto spm = Game() ? At<uintptr_t>(Game() + 0x54) : 0;
    if (g_kept && spm == g_kept) {
        memcpy(reinterpret_cast<void*>(spm + 0x568), g_limits, sizeof g_limits);
        if (!g_cheat) At<unsigned>(spm + 0x62c) &= ~2u;
    }
    g_kept = 0;
}
static void Powers() {
    auto spm = At<uintptr_t>(Game() + 0x54);
    if (!spm) return;
    if (g_kept != spm) {
        g_kept = spm;
        memcpy(g_limits, reinterpret_cast<void*>(spm + 0x568), sizeof g_limits);
        g_cheat = (At<unsigned>(spm + 0x62c) & 2) != 0;
    }
    At<unsigned>(spm + 0x62c) |= 2;
    auto limit = reinterpret_cast<unsigned*>(spm + 0x568), count = reinterpret_cast<unsigned*>(spm + 0x5b4);
    static unsigned used[19];
    for (int i = 1; i <= 18; i++) {
        limit[i] = 1000;
        if (count[i] != used[i]) Log("[freeplay] power %d, count %u to %u", i, used[i], count[i]);
        used[i] = count[i];
    }
    static unsigned flags[4];
    for (int i = 0; i < 4; i++) {
        auto s = Slot(spm, i);
        if (At<int>(s) == -1) Fill(spm, i, kPowers[i].power);
        unsigned f = At<unsigned>(s + 0x14);
        if (f != flags[i]) Log("[freeplay] slot %d: power %d, flags %x, time %.1f", i + 1, At<int>(s), f, At<float>(s + 8));
        flags[i] = f;
    }
    if (g_pending >= 0) {
        if (At<unsigned>(Slot(spm, g_pending) + 0x14) & 1) { Switch(g_pending); g_pending = -1; }
        else if (--g_wait <= 0) { Log("[freeplay] slot %d never got ready", g_pending + 1); g_pending = -1; }
    }
    if (g_power >= 0) Press(spm, g_power);
    g_power = -1;
}
// The statues empty every slot that has no village behind it, each update.
typedef void(__fastcall* SetSlotT)(uintptr_t, void*, int, int, void*);
static SetSlotT oSetSlot;
static void __fastcall hSetSlot(uintptr_t self, void* edx, int slot, int power, void* ref) {
    if (!g_active) oSetSlot(self, edx, slot, power, ref);
}

// "Is this the last story map?" Yes for the powers only, not for goals, villagers, totems and the
// ending.
typedef int(__cdecl* LastT)();
static LastT oLast;
static int __cdecl Last(uintptr_t from) {
    static const uintptr_t powers[] = { 0x1008a4ff, 0x1008b01a, 0x10127e2c, 0x10128b35, 0x10128d8d, 0x1012e0d2, 0x1012e607 };
    for (auto p : powers) {
        if (from == Dust(p)) return 1;
    }
    return oLast();
}
// The original leaves edx and the xmm registers alone.
static __declspec(naked) void hLast() {
    __asm {
        cmp g_active, 0
        je original
        push edx
        sub esp, 128
        movups [esp], xmm0
        movups [esp + 16], xmm1
        movups [esp + 32], xmm2
        movups [esp + 48], xmm3
        movups [esp + 64], xmm4
        movups [esp + 80], xmm5
        movups [esp + 96], xmm6
        movups [esp + 112], xmm7
        push dword ptr [esp + 132]
        call Last
        add esp, 4
        movups xmm0, [esp]
        movups xmm1, [esp + 16]
        movups xmm2, [esp + 32]
        movups xmm3, [esp + 48]
        movups xmm4, [esp + 64]
        movups xmm5, [esp + 80]
        movups xmm6, [esp + 96]
        movups xmm7, [esp + 112]
        add esp, 128
        pop edx
        ret
    original:
        jmp oLast
    }
}

// ---- rules and events ---------------------------------------------------------------------------
// [game+28] sequence manager: sequences +4, count +8, each +1c bit 0 = on. Its update (100a23d0) runs
// them and then the actions started directly (100a2640). Actions are made by name (10099640);
// vtable +8 sets the position, +c is the duration.
// The random sources and tsunamis need settings that maps may lack, so these use those of the powers.
static const struct { const char* name; bool placed; const char* label; } kEvents[] = {
    { "GlobalRain", false, "Rain" }, { "RandomVolcano", true, "Volcano" }, { "RandomCaldeira", true, "Caldera" },
    { "RandomCrack", true, "Crack" }, { nullptr, true, "Water source" }, { nullptr, true, "Lava source" }, { "Tsunami", false, "Tsunami" },
};
enum { EVENTS = sizeof kEvents / sizeof *kEvents, CLEAR = EVENTS, RAIN = 0, WATER = 4, LAVA = 5, TSUNAMI = 6 };
// Started actions, and sources by their handle. The manager drops actions from its list (+18, count
// +1c) when they end, but nobody deletes them, so the pointers stay valid.
static struct { uintptr_t action; unsigned handle; int kind; } g_run[64];
static int g_runs;
static uintptr_t Entity(unsigned handle) {
    return ((uintptr_t(__thiscall*)(uintptr_t, unsigned*))Dust(0x1000fd00))(At<uintptr_t>(Dust(0x10180a58)), &handle);
}
static void Prune(uintptr_t manager) {
    auto list = At<uintptr_t*>(manager + 0x18);
    auto n = At<unsigned>(manager + 0x1c);
    int kept = 0;
    for (int i = 0; i < g_runs; i++) {
        bool runs = false;
        if (g_run[i].action) {
            for (unsigned k = 0; list && k < n; k++) runs |= list[k] == g_run[i].action;
        } else runs = Entity(g_run[i].handle) != 0;
        if (runs) g_run[kept++] = g_run[i];
    }
    g_runs = kept;
}
static void Clear(uintptr_t manager) {
    Prune(manager);
    Log("[freeplay] stopping %d events", g_runs);
    auto spm = At<uintptr_t>(Game() + 0x54);
    for (int i = 0; i < g_runs; i++) {
        __try {
            if (g_run[i].action) ((void(__thiscall*)(uintptr_t, uintptr_t))Dust(0x100a2350))(manager, g_run[i].action);   // stops and removes it
            else if (auto e = Entity(g_run[i].handle)) {
                At<unsigned>(e + 0xf4) |= 8;                  // remove
                auto& count = At<unsigned>(spm + 0x5b4 + (g_run[i].kind == WATER ? 8 : 4) * 4);
                if (count) count--;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[freeplay] stopping %s raised %08x", kEvents[g_run[i].kind].label, GetExceptionCode());
        }
    }
    g_runs = 0;
}
// Settings of the source powers, manager+204 (lava) and +214 (water): flow, and three numbers for
// what comes out.
static uintptr_t Source(int n, float* pos) {
    auto spm = At<uintptr_t>(Game() + 0x54);
    if (!spm) return 0;
    auto p = reinterpret_cast<int*>(spm + (n == WATER ? 0x214 : 0x204));
    Log("[freeplay] %s at %.1f %.1f %.1f: flow %.3f, %d %d %d", kEvents[n].label, pos[0], pos[1], pos[2], *(float*)p, p[1], p[2], p[3]);
    auto e = ((uintptr_t(__thiscall*)(uintptr_t, int*, float*))Dust(0x1012ace0))(spm, p, pos);
    if (e) At<unsigned>(spm + 0x5b4 + (n == WATER ? 8 : 4) * 4)++;
    return e;
}
// Amount, height and side of the tsunami power, manager+32c. Sides are 1 to 8 around the map.
static void Tsunami(uintptr_t a, float* pos) {
    static const int side[3][3] = { { 1, 2, 3 }, { 8, 0, 4 }, { 7, 6, 5 } };
    auto spm = At<uintptr_t>(Game() + 0x54);
    float r = sqrtf(pos[0] * pos[0] + pos[1] * pos[1]), x = r > 1 ? pos[0] / r : 0, y = r > 1 ? pos[1] / r : 1;
    int sx = x < -0.38f ? 0 : x > 0.38f ? 2 : 1, sy = y < -0.38f ? 0 : y > 0.38f ? 2 : 1;
    At<float>(a + 0xc) = 50;
    At<int>(a + 0x1c) = spm ? At<int>(spm + 0x32c) : 0;
    At<float>(a + 0x20) = spm ? At<float>(spm + 0x330) : 0;
    At<int>(a + 0x24) = side[sx][sy];
    At<unsigned>(a + 0x28) |= 1;
    Log("[freeplay] tsunami from side %d, %d, height %.2f, the map's side %d", side[sx][sy], At<int>(a + 0x1c), At<float>(a + 0x20), spm ? At<int>(spm + 0x334) : 0);
}
static void Launch(uintptr_t manager, int n) {
    Prune(manager);
    if (g_runs == sizeof g_run / sizeof *g_run) { Log("[freeplay] too many events"); return; }
    __try {
        float pos[3];
        if (!Position(pos)) return;
        if (!kEvents[n].name) {
            auto e = Source(n, pos);
            if (!e) return;
            g_run[g_runs].action = 0;
            g_run[g_runs].handle = At<unsigned>(e + 0x54);
            g_run[g_runs++].kind = n;
            return;
        }
        auto a = ((uintptr_t(__cdecl*)(const char*))Dust(0x10099640))(kEvents[n].name);
        Log("[freeplay] event %s at %.1f %.1f %.1f%s", kEvents[n].name, pos[0], pos[1], pos[2], a ? "" : ", unknown");
        if (!a) return;
        if (kEvents[n].placed) ((void(__thiscall*)(uintptr_t, float*))At<uintptr_t>(At<uintptr_t>(a) + 8))(a, pos);
        if (n == RAIN) {                         // seconds, amount per second
            At<float>(a + 0xc) = 20;
            At<float>(a + 0x1c) = 0.05f;
        }
        if (n == TSUNAMI) Tsunami(a, pos);
        ((void(__thiscall*)(uintptr_t, uintptr_t))Dust(0x100a2640))(manager, a);
        g_run[g_runs].action = a;
        g_run[g_runs++].kind = n;
        Log("[freeplay] duration %.1f", At<float>(a + 0xc));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[freeplay] event %s raised %08x", kEvents[n].label, GetExceptionCode());
    }
}

// ---- villagers and their landmarks -----------------------------------------------------------------
// Entities with flag +f4 bit 3 are removed in their own update. Knowledge totems lack that and are
// removed by the function that does it, vtable +48. Without the minimum of villagers the game would be
// lost, unless the species has +528 bit 5. The display's elements are bits at +8c: 0 villagers and
// villages, 1 vegetation, 2 coming events.
static const struct { int species; unsigned entity; const char* name; } kPeople[] = {
    { 0x10080001, 0x20080001, "villagers" }, { 0x10200007, 0x20200007, "totems" }, { 0x1020000e, 0x2020000e, "knowledge totems" },
    { 0x10200008, 0x20200008, "passages" }, { 0x10200006, 0x20200006, "call points" }, { 0x10200009, 0x20200009, "village sites" },
    { 0x10800001, 0x20800001, "villages" }, { 0x10800002, 0x20800002, "huts" }, { 0x1020000a, 0x2020000a, "ruins" },
    { 0x10200001, 0x20200001, "knowledge stones" },
};
enum { HUMAN = 0, KNOWLEDGE = 2, HUD_BITS = 7 };
static bool g_people;                // false when removing them failed
static unsigned g_species, g_shownHud;
static bool g_hud;
static uintptr_t Hud() {
    auto holder = At<uintptr_t>(Game() + 0x2b8);
    return holder ? ((uintptr_t(__thiscall*)(uintptr_t, int))Dust(0x100b9f90))(holder, 0) : 0;
}
static void Remove(int kind) {
    auto manager = At<uintptr_t>(Dust(0x1019aed8));
    unsigned handle = At<unsigned>(Dust(0x10180a54));                // none
    ((void(__thiscall*)(uintptr_t, int, unsigned*))Dust(0x10013950))(manager, kPeople[kind].species, &handle);
    auto species = Entity(handle);
    if (!species) return;
    if (kind == HUMAN) {
        g_species = handle;
        At<unsigned>(species + 0x528) |= 0x20;
    }
    unsigned n = 0, copy[256];
    auto list = ((unsigned*(__thiscall*)(uintptr_t, unsigned*, unsigned*))Dust(0x1007c900))(At<uintptr_t>(manager + 0x40), &handle, &n);
    if (!list) return;
    if (n > 256) n = 256;
    memcpy(copy, list, n * sizeof *list);
    int gone = 0;
    for (unsigned i = 0; i < n; i++) {
        auto e = Entity(copy[i]);
        if (!e || At<unsigned>(e + 0x3c) != kPeople[kind].entity || (At<unsigned>(e + 0xf4) & 10)) continue;
        if (kind == KNOWLEDGE) {
            auto effect = At<int>(e + 0x1f4);                        // its light
            auto effects = At<uintptr_t>(Game() + 0x5c);
            if (effects && effect >= 0 && effect <= 0x31) {
                if (auto x = At<uintptr_t>(effects + 0x7b0 + effect * 4)) At<unsigned>(x + 0x20) |= 1;
                At<int>(e + 0x1f4) = -1;
            }
            ((void(__thiscall*)(uintptr_t))At<uintptr_t>(At<uintptr_t>(e) + 0x48))(e);
        } else At<unsigned>(e + 0xf4) |= 8;
        gone++;
    }
    if (gone) Log("[freeplay] %d %s removed", gone, kPeople[kind].name);
}
static void Villagers() {
    static int kind;
    if (!g_people) return;
    __try {
        for (kind = 0; kind < (int)(sizeof kPeople / sizeof *kPeople); kind++) Remove(kind);
        if (auto hud = Hud()) {
            if (!g_hud) g_shownHud = At<unsigned>(hud + 0x8c) & HUD_BITS;
            g_hud = true;
            At<unsigned>(hud + 0x8c) &= ~HUD_BITS;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_people = false;
        Log("[freeplay] removing %s raised %08x", kind < (int)(sizeof kPeople / sizeof *kPeople) ? kPeople[kind].name : "the display", GetExceptionCode());
    }
}
static void RestorePeople() {
    __try {
        auto hud = g_hud && Game() ? Hud() : 0;
        if (hud) At<unsigned>(hud + 0x8c) |= g_shownHud;
        auto species = g_species ? Entity(g_species) : 0;
        if (species && At<unsigned>(species + 0x3c) == 0x10080001) At<unsigned>(species + 0x528) &= ~0x20u;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[freeplay] display not restored");
    }
    g_hud = false;
    g_species = 0;
    g_people = true;
}

// ---- panel ---------------------------------------------------------------------------------------
static PanelLine g_lines[40];
static int g_lineCount;
static void Line(int style, const char* key, const char* text, ...) {
    if (g_lineCount == sizeof g_lines / sizeof *g_lines) return;
    auto& l = g_lines[g_lineCount++];
    memset(&l, 0, sizeof l);
    l.style = style;
    strncpy(l.key, key, sizeof l.key - 1);
    va_list a;
    va_start(a, text);
    _vsnprintf(l.text, sizeof l.text - 1, text, a);
    va_end(a);
}
static void Panel(uintptr_t manager) {
    auto spm = At<uintptr_t>(Game() + 0x54);
    if (!g_panel || !spm) return;
    g_lineCount = 0;
    Line(PANEL_DIM, "F4", "hide or show this");
    Line(PANEL_HEAD, "", "Powers");
    int active = -1;
    for (auto& p : kPowers) {
        bool on = false;
        for (int i = 0; i < 4; i++) on |= At<int>(Slot(spm, i)) == p.power && (At<unsigned>(Slot(spm, i) + 0x14) & 2);
        if (on) active = p.power;
        Line(on ? PANEL_ON : PANEL_TEXT, p.key, on ? "%s  (on)" : "%s", p.label);
    }
    Line(PANEL_TEXT, "", "");
    Line(PANEL_HEAD, "", "Events (number pad)");
    Prune(manager);
    for (int i = 0; i < EVENTS; i++) {
        int n = 0;
        for (int k = 0; k < g_runs; k++) n += g_run[k].kind == i;
        char key[2] = { (char)('1' + i), 0 };
        if (n) Line(PANEL_ON, key, "%s  (%d)", kEvents[i].label, n);
        else Line(PANEL_TEXT, key, "%s", kEvents[i].label);
    }
    Line(g_runs ? PANEL_TEXT : PANEL_DIM, "0", "stop all events");
    for (auto& u : kUses) {
        if (u.power != active) continue;
        Line(PANEL_TEXT, "", "");
        Line(PANEL_HEAD, "", "Use it");
        for (int i = 0; i < 4 && u.key[i]; i++) Line(PANEL_ON, u.key[i], "%s", u.text[i]);
    }
    PanelShow(g_lines, g_lineCount);
}

typedef void(__fastcall* SequenceT)(uintptr_t, void*, float);
static SequenceT oSequence;
static void __fastcall hSequence(uintptr_t self, void* edx, float dt) {
    if (g_active) {
        auto list = At<uintptr_t*>(self + 4);
        auto n = At<unsigned>(self + 8);
        for (unsigned i = 0; list && i < n && i < 64; i++) At<unsigned>(list[i] + 0x1c) &= ~1u;
        auto cursor = At<uintptr_t>(Dust(0x1018d5d0));
        if (State() == 7 && cursor) {
            if (!g_ready) {
                g_ready = true;
                Log("[freeplay] %u sequences off, level flags %x, layers %x", n, At<unsigned>(At<uintptr_t>(Game() + 0x18) + 0x1c0), At<unsigned>(cursor + 0x7dc));
                for (int i = 1; i <= 10; i++) ((void(__thiscall*)(uintptr_t, int, int))Dust(0x10035000))(cursor, i, 1);   // may pick up everything
                Log("[freeplay] layers now %x", At<unsigned>(cursor + 0x7dc));
            }
            Powers();
            Villagers();
            if (g_event == CLEAR) Clear(self);
            else if (g_event >= 0) Launch(self, g_event);
            Panel(self);
        }
        g_event = -1;
    }
    oSequence(self, edx, dt);
}

// ---- maps ----------------------------------------------------------------------------------------
// [game+50] story +1c, count +20, challenges +2c, count +30, both 28 bytes each (file name, name
// in the menu, text). Progress of challenges: [[game+2c0]+14], 20 bytes each, 0 = locked.
static uint8_t g_progress[64 * 20];
struct Maps {                        // story maps in place of the challenges
    uintptr_t list = 0, profile = 0, maps = 0, progress = 0;
    unsigned count = 0;
    Maps() {
        auto g = Game();
        auto l = At<uintptr_t>(g + 0x50), p = At<uintptr_t>(g + 0x2c0);
        if (!l || !p) return;
        auto stories = At<unsigned>(l + 0x20);
        if (!At<uintptr_t>(l + 0x1c) || !stories || stories > 64 || stories > At<unsigned>(l + 0x30)) return;
        list = l;
        profile = p;
        maps = At<uintptr_t>(l + 0x2c);
        count = At<unsigned>(l + 0x30);
        progress = At<uintptr_t>(p + 0x14);
        memset(g_progress, 0, sizeof g_progress);
        for (int i = 0; i < 64; i++) g_progress[i * 20] = 1;
        At<uintptr_t>(l + 0x2c) = At<uintptr_t>(l + 0x1c);
        At<unsigned>(l + 0x30) = stories;
        At<uintptr_t>(p + 0x14) = (uintptr_t)g_progress;
    }
    ~Maps() {
        if (!list) return;
        At<uintptr_t>(list + 0x2c) = maps;
        At<unsigned>(list + 0x30) = count;
        At<uintptr_t>(profile + 0x14) = progress;
    }
};

typedef void(__fastcall* ListT)(uintptr_t, void*);
static ListT oList;
static void __fastcall hList(uintptr_t self, void* edx) {
    if (!g_list) { oList(self, edx); return; }
    Maps m;
    if (!m.list) Log("[freeplay] story maps not found");
    oList(self, edx);
}

typedef void(__cdecl* StartT)(int, int, int);
static StartT oStart;
static void __cdecl hStart(int index, int story, int resume) {
    g_active = g_list && !story;
    Restore();
    RestorePeople();
    if (!g_active) { oStart(index, story, resume); return; }
    Maps m;
    if (m.list) {
        g_playing = g_ready = false;
        g_runs = 0;
        g_power = g_pending = g_event = -1;
        auto name = At<const char*>(At<uintptr_t>(m.list + 0x2c) + index * 28);
        Log("[freeplay] starting map %d: %s", index + 1, name ? name : "");
    } else g_active = false;
    oStart(index, story, resume);
}

// Events on a map unlock challenges.
typedef int(__fastcall* UnlockT)(uintptr_t, void*, int);
static UnlockT oUnlock;
static int __fastcall hUnlock(uintptr_t self, void* edx, int id) { return g_active ? 0 : oUnlock(self, edx, id); }

// Forgets the four power requests of the game's listener (+18) when a map ends. A request whose event
// is already destroyed is null, and the original crashed on it.
static void __fastcall hForget(uintptr_t self, void*) {
    for (int i = 0; i < 4; i++) {
        auto& ref = At<uintptr_t>(self + 0x18 + i * 4);
        __try {
            auto request = ref ? At<uintptr_t>(ref + 0x1c) : 0;
            if (request) At<unsigned>(request + 0x24) &= ~1u;
            if (ref) At<int>(ref + 0x20)--;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[freeplay] power request %d was gone", i + 1);
        }
        ref = 0;
    }
}

// Main menu: F3 lists the story maps in the challenge menu, or the challenges. In a sandbox map: F4
// hides the panel, 1 to = are powers, number pad 1 to 7 natural events, 0 stops them.
void FreeplayFrame(bool fg) {
    enum { EVENT_KEYS = 2, POWER_KEYS = EVENT_KEYS + EVENTS + 1, KEYS = POWER_KEYS + POWERS };
    static bool was[KEYS];
    if (!g_on || !oStart || !Game()) return;
    int s = State();
    if (s == 7) g_playing = true;
    if (s != 7) g_ready = false;                   // another map, or this one again
    if (s == 4 && g_playing && g_active) {
        g_active = g_playing = false;
        Restore();
        RestorePeople();
        Log("[freeplay] left");
    }
    for (int i = 0; i < KEYS; i++) {
        int vk = i == 0 ? VK_F3 : i == 1 ? VK_F4 : i >= POWER_KEYS ? kPowers[i - POWER_KEYS].vk :
                 i == POWER_KEYS - 1 ? VK_NUMPAD0 : VK_NUMPAD1 + i - EVENT_KEYS;
        bool down = fg && (GetAsyncKeyState(vk) & 0x8000), press = down && !was[i];
        was[i] = down;
        if (!press) continue;
        if (i == 0 && s == 4) {
            g_list = !g_list;
            Log("[freeplay] challenge menu shows %s", g_list ? "story maps" : "challenges");
        }
        if (!g_active || s != 7 || SandboxActive()) continue;
        if (i == 1) { g_panel = !g_panel; if (!g_panel) PanelHide(); Log("[freeplay] panel %s", g_panel ? "on" : "off"); }
        else if (i >= POWER_KEYS) g_power = i - POWER_KEYS;
        else if (i >= EVENT_KEYS) g_event = i - EVENT_KEYS;     // the last one is CLEAR
    }
    if (s == 4) {
        PanelLine l = {};
        l.style = g_list ? PANEL_ON : PANEL_TEXT;
        strcpy(l.key, "F3");
        strcpy(l.text, g_list ? "Sandbox: on, choose a map in Challenges" : "Sandbox: off");
        PanelShow(&l, 1);
    }
}

bool FreeplayActive() { return g_active; }
void FreeplayInit() {
    g_on = IniInt("Sandbox", "Sandbox", 1);
    if (g_on) PanelInit();
}
void FreeplayDustLoaded() {
    oStart = nullptr;
    g_list = g_active = g_hud = false;
    g_species = 0;
    g_kept = 0;
    if (!g_on) return;
    static const struct { uintptr_t va; const char* bytes; } called[] = {
        { 0x10127b50, "51 8b 44 24 08 8b 00 0f 57 c0 f3 0f 11 04 24" }, { 0x10099640, "56 8b 74 24 08 b9 ?? ?? ?? ?? 8b c6" },
        { 0x100a2640, "56 8b 74 24 08 8b 06 8b 50 04 57 8b f9 8b ce" }, { 0x10035000, "8b 54 24 04 4a 83 fa 09 0f 87 7d 01 00 00" },
        { 0x1012ace0, "83 ec 10 8d 44 24 04 50 8d 4c 24 0c 51 8d 54 24" }, { 0x1000fd00, "56 8b 74 24 08 8b 16 83 e2 03 33 c0 83 fa 01 7d" },
        { 0x10013950, "51 8b 44 24 0c 8b 54 24 08 8b 49 40 50 52" }, { 0x1007c900, "8b 44 24 04 8b 10 8b c2 c1 ea 02" },
        { 0x100b9f90, "83 ec 08 53 55 56 57 8d 71 20" },
        { 0x100a2350, "56 57 8b f9 8b 77 18 8b 47 1c 8b ce 8d 14 81 3b" },
    };
    for (auto& c : called) if (!BytesMatch(Dust(c.va), c.bytes)) { Log("[freeplay] code differs, off"); return; }
    oUnlock = (UnlockT)Hook("challenge unlock", Dust(0x10135190), "8b c1 8b 0d ?? ?? ?? ?? 8b 51 74 f6 82 0c 01 00 00 02", 8, (void*)hUnlock);
    oSetSlot = (SetSlotT)Hook("power slot", Dust(0x1012aaa0), "56 8b 74 24 10 8b 06 83 e0 03 83 f8 01", 5, (void*)hSetSlot);
    oSequence = (SequenceT)Hook("sequences", Dust(0x100a23d0), "57 8b f9 83 bf c8 00 00 00 00 0f 85", 10, (void*)hSequence);
    oLast = (LastT)Hook("last map test", Dust(0x1013bc60), "a1 ?? ?? ?? ?? 8b 48 18 f6 81 c0 01 00 00 02", 8, (void*)hLast);
    oList = (ListT)Hook("challenge list", Dust(0x1001d3d0), "81 ec d0 00 00 00 56 8b f1 83 7e 2c 00", 6, (void*)hList);
    oSwitch = (SwitchT)Hook("power switch", Dust(0x100149e0), "83 ec 48 53 8b 5c 24 50 56 57 8b f9", 8, (void*)hSwitch);
    Hook("power requests", Dust(0x10014ae0), "8b 41 18 56 57 83 cf ff 33 d2 8d 77 ff 3b c2 74", 5, (void*)hForget);
    if (!oSwitch || !oUnlock || !oSetSlot || !oSequence || !oLast || !oList) return;
    oStart = (StartT)Hook("map start", Dust(0x1001a580), "81 ec 0c 01 00 00 83 bc 24 14 01 00 00 00 56 57 0f 84 5b 01 00 00", 6, (void*)hStart);
}
