// World editing:
//   UnlockMaps  - lists every story map and challenge; the save's progress is not changed
//   EditorKeys  - numpad keys that use the last story map's powers at the cursor, on any map
#include "common.h"

static int g_unlock, g_keys, g_limits;
static bool g_bad;

static __declspec(naked) void StoryStub() {
    __asm {
        mov eax, [ecx]
        test eax, eax
        jnz have
        inc eax
    have:
        inc eax
        cmp dword ptr [ecx + 4], 0
        ret
    }
}
static __declspec(naked) void ChallengeStub() {
    __asm {
        mov eax, [ebx + ecx]
        test eax, eax
        jnz have
        inc eax
    have:
        mov [esp + 0x38], eax
        ret
    }
}
static void CallPatch(const char* name, uintptr_t va, const char* expect, void* stub, int n) {
    if (!BytesMatch(va, expect)) { Log("[editor] %s differs, skipped", name); return; }
    uint8_t b[16];
    memset(b, 0x90, sizeof b);
    b[0] = 0xE8;
    *(int32_t*)(b + 1) = (int32_t)((uintptr_t)stub - (va + 5));
    WriteMem(va, b, n);
    Log("[editor] %s ok", name);
}

void EditorInit() {
    g_unlock = IniInt("Editor", "UnlockMaps", 0);
    g_keys = IniInt("Editor", "EditorKeys", 0);
    g_limits = IniInt("Editor", "RaiseLimits", 1);
}
void EditorDustLoaded() {
    g_bad = false;
    if (!g_unlock) return;
    CallPatch("story map list", Dust(0x1001d724), "8b 01 40 83 79 04 00 74 03", (void*)StoryStub, 7);
    CallPatch("challenge list", Dust(0x1001a990), "8b 04 0b 89 44 24 34 85 c0", (void*)ChallengeStub, 7);
}

static const struct { int vk, id; const char* name; } kTools[] = {
    { VK_NUMPAD1, 1, "big mountain" }, { VK_NUMPAD2, 15, "small mountain" }, { VK_NUMPAD3, 7, "crater" },
    { VK_NUMPAD4, 4, "lava source" },  { VK_NUMPAD5, 5, "volcano" },         { VK_NUMPAD6, 6, "remove volcano or lava source" },
    { VK_NUMPAD7, 8, "water source" }, { VK_NUMPAD8, 12, "remove water source" }, { VK_NUMPAD9, 14, "tree" },
    { VK_NUMPAD0, 16, "remove tree" }, { VK_DECIMAL, 10, "tsunami" },        { VK_MULTIPLY, 11, "tides" },
    { VK_ADD, 18, "add vegetation" },  { VK_SUBTRACT, 17, "remove vegetation" }, { VK_DIVIDE, 3, "big caldera" },
};

static void Run(int id, const char* name) {
    auto g = Game();
    if (!g || !BytesMatch(Dust(0x1012b640), "55 8b ec 83 e4 f8 83 ec 44")) return;
    auto flow = At<uintptr_t>(g + 0x74), spm = At<uintptr_t>(g + 0x54), cursor = At<uintptr_t>(Dust(0x1018d5d0));
    if (!flow || At<int>(flow + 4) != 7 || !spm || !cursor) { Log("[editor] %s ignored, not in play", name); return; }
    __try {
        auto obj = At<uintptr_t>(cursor + 0xC);
        if (!obj) return;
        float* p = ((float*(__thiscall*)(uintptr_t))At<uintptr_t>(At<uintptr_t>(obj) + 0xC))(obj);
        float pos[3] = { p[0], p[1], p[2] };
        unsigned &limit = At<unsigned>(spm + 0x568 + id * 4), count = At<unsigned>(spm + 0x5b4 + id * 4);
        Log("[editor] %s (id %d) at %.1f %.1f %.1f, count %u limit %u", name, id, pos[0], pos[1], pos[2], count, limit);
        if (g_limits && limit < 64) limit = 64;
        ((void(__thiscall*)(uintptr_t, int, float*))Dust(0x1012b640))(spm, id, pos);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_bad = true;                           // the map lacks the data
        Log("[editor] %s raised %08x, editor keys off until reload", name, GetExceptionCode());
    }
}

void EditorFrame(bool fg) {
    static bool was[32];
    if (!g_keys) return;
    for (int i = 0; i < (int)(sizeof kTools / sizeof *kTools); i++) {
        bool down = fg && (GetAsyncKeyState(kTools[i].vk) & 0x8000);
        if (down && !was[i] && !g_bad) Run(kTools[i].id, kTools[i].name);
        was[i] = down;
    }
}
