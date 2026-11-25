// Skipping of videos and in-engine cinematics. Cinematics run by a script (entering a map, the
// Breath's introduction, the end of a migration or of the game) also advance the game, so they
// are fast-forwarded; all others are ended.
#include "common.h"

static volatile bool g_videoSkippable;
static volatile bool g_logo;
static volatile DWORD g_autoStop;  // tick at which a boot logo started, 0 when none
static int g_skipIntro;
static float g_stillTime = 0.1f;
static bool g_skip;                 // skip requested
static uintptr_t g_scripted;        // object of the running scripted cinematic
static const char* g_scriptedName = "";
static int g_frames;                // updates since the current cinematic started

// ---- videos --------------------------------------------------------------------
// Loading-screen videos are not skippable, they cover background loading.
typedef void(__fastcall* PlayT)(void*, void*, const char*, int);
static PlayT oPlay;
static void __fastcall hPlay(void* self, void* edx, const char* name, int flag) {
    static const char* ok[] = { "Beginning", "PassageLong", "PassageShort", "dolby", "TrailerDE",
                                "TrailerES", "TrailerFR", "TrailerIT", "TrailerUS" };
    bool s = false;
    for (auto k : ok)
        if (!_stricmp(k, name)) s = true;
    bool logo = !_stricmp(name, "ubisoft") || !_stricmp(name, "dolby");
    if (logo) s = true;
    g_videoSkippable = s;
    g_autoStop = GetTickCount();
    g_logo = logo && g_skipIntro;
    Log("[video] play %s%s", name, s ? " (skippable)" : "");
    oPlay(self, edx, name, flag);
}

static bool SkipVideo() {
    auto mgr = At<uintptr_t>(0x924bf0);   // bink::BinkPlayerInterface
    if (!mgr || !g_videoSkippable) return false;
    auto vt = At<uintptr_t>(mgr);
    auto cur = ((void*(__thiscall*)(uintptr_t))At<uintptr_t>(vt + 0x2c))(mgr);
    if (!cur || !At<uintptr_t>((uintptr_t)cur)) return false;   // not open yet
    ((void(__thiscall*)(uintptr_t, void*))At<uintptr_t>(vt + 0x24))(mgr, cur);   // stop
    g_videoSkippable = false;
    Log("[video] skipped");
    return true;
}

// ---- in-engine cinematics ----------------------------------------------------------
// Player: +0 playing id (-1 idle), +8 object.
static uintptr_t CinePlayer() {
    auto g = Game();
    auto scene = g ? At<uintptr_t>(g + 0x64) : 0;   // GameSceneManager
    return scene ? At<uintptr_t>(scene + 0x3ac) : 0;
}
static int FlowState() {
    auto g = Game();
    auto f = g ? At<uintptr_t>(g + 0x74) : 0;
    return f ? At<int>(f + 4) : -1;
}
static void FastForward(bool on) {
    static bool cur;
    if (on == cur) return;
    cur = on;
    At<float>(0x90c828) = on ? 10.0f : 1.0f;   // engine clamps the result to 1/6 s per frame
    Log("[cinematic] fast-forward %s", on ? "on" : "off");
}

// Cinematic player update. The id of a finished cinematic is cleared at its start, so act after it.
typedef void(__fastcall* UpdT)(uintptr_t, void*, float);
static UpdT oUpd;
static void __fastcall hUpd(uintptr_t p, void* edx, float dt) {
    static int last = -1;
    oUpd(p, edx, dt);
    int id = At<int>(p);
    if (id != last) {
        Log("[cinematic] current %d", id);
        last = id;
        g_frames = 0;
        g_scripted = 0;
        g_skip = false;
    }
    if (id < 0) { g_skip = false; FastForward(false); return; }
    g_frames++;
    if (!g_skip || g_frames < 5 || FlowState() != 7) return;
    if (g_scripted && g_scripted == At<uintptr_t>(p + 8)) { FastForward(true); return; }
    auto scene = At<uintptr_t>(Game() + 0x64);
    if (At<int>(scene + 0x3b4) == -1) At<int>(scene + 0x3b0) = id;
    ((void(__thiscall*)(uintptr_t, int))Dust(0x10096800))(p, id);
    Log("[cinematic] skipped %d", id);
    g_skip = false;
}

// Notes the object (+0xC) of a scripted cinematic.
template <int N> struct Scripted {
    static UpdT orig;
    static const char* name;
    static void __fastcall hook(uintptr_t self, void* edx, float dt) {
        auto p = CinePlayer();
        auto obj = At<uintptr_t>(self + 0xC);
        if (p && At<int>(p) >= 0 && obj && obj == At<uintptr_t>(p + 8) && g_scripted != obj) {
            g_scripted = obj;
            Log("[cinematic] %d is scripted (%s)", At<int>(p), name);
        }
        orig(self, edx, dt);
    }
    static void install(const char* n, uintptr_t va, const char* expect) {
        name = n;
        orig = (UpdT)Hook(n, Dust(va), expect, 6, (void*)hook);
    }
};
template <int N> UpdT Scripted<N>::orig;
template <int N> const char* Scripted<N>::name;

// ---- boot still --------------------------------------------------------------------
// The boot still covers mounting of the archives. Opening the logo before that crashes in the
// file lookup, so wait until the lookup would succeed, at most as long as the original.
static bool ArchivesReady() {
    auto m = At<uintptr_t>(0x924be8);
    if (!m) return false;
    auto list = At<uintptr_t>(m + 0x68);
    auto n = At<unsigned>(m + 0x6c);
    if (!list || !n || n > 64) return false;
    // Same order as the game's lookup (832bd0).
    auto find = (uintptr_t(__thiscall*)(uintptr_t, uint32_t))0x8318f0;
    for (unsigned i = n; i-- > 0;) {
        auto a = At<uintptr_t>(list + 8 * i + 4);
        if (!a || !At<uintptr_t>(a + 4)) return false;
        if (find(a, 0x1dc0299f) || find(a, 0x1dc029a0)) return true;   // Ubisoft_25 / Ubisoft_30
    }
    return false;
}
// Archive lookup: an archive without a table does not contain the file.
typedef uintptr_t(__fastcall* FindT)(uintptr_t, void*, uint32_t);
static FindT oFind;
static uintptr_t __fastcall hFind(uintptr_t archive, void* edx, uint32_t key) {
    if (!At<uintptr_t>(archive + 4)) return 0;
    return oFind(archive, edx, key);
}
typedef int(__fastcall* DoneT)(uintptr_t, void*);
static DoneT oDone;
static int __fastcall hDone(uintptr_t self, void* edx) {   // "has the video finished?"
    static bool open;
    static DWORD first, ready;
    if (!open) {
        DWORD now = GetTickCount();
        if (!first) first = now ? now : 1;
        bool ok = false;
        __try {
            ok = ArchivesReady();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (!ok) ready = 0;
        else if (!ready) ready = now ? now : 1;
        if (ready && now - ready >= 250) {
            open = true;
            Log("[video] archives ready after %u ms", now - first);
        } else if (now - first >= 6000) {
            open = true;
            Log("[video] archives not seen ready, using the original wait");
        } else return 0;
    }
    return oDone(self, edx);
}

void CutscenesInit() {
    oFind = (FindT)Hook("archive lookup", 0x8318f0, "53 55 8b 6c 24 0c 8b c5", 6, (void*)hFind);
    g_skipIntro = cfg.skipVideos && IniInt("Cutscenes", "SkipIntro", 1);
    // Still shown for 5 seconds before the logo.
    if (g_skipIntro && BytesMatch(0x81b761, "d9 05 58 eb 89 00 89 5e 10") &&
        (oDone = (DoneT)Hook("boot sequence", 0x81abc0, "56 8b f1 8b 0d f0 4b 92 00 8b 01", 9, (void*)hDone)) != nullptr) {
        uintptr_t p = (uintptr_t)&g_stillTime;
        WriteMem(0x81b763, &p, 4);
        Log("[video] boot still shortened");
    }
    if (cfg.skipVideos)   // push ecx; mov ecx,[938614]
        oPlay = (PlayT)Hook("video play", 0x81a8a0, "51 8b 0d 14 86 93 00", 7, (void*)hPlay);
}
void CutscenesDustLoaded() {
    if (!cfg.skipCinematics || !BytesMatch(Dust(0x10096800), "56 8b f1 83 3e ff 0f 8e")) return;
    Scripted<0>::install("EnterMap", 0x100b1720, "83 ec 38 56 8b f1");
    Scripted<1>::install("EndMigration", 0x100b0840, "83 ec 1c 53 55 56 8b f1");
    Scripted<2>::install("SphereOrigin", 0x100b3960, "f3 0f 10 44 24 04 83 ec 30");
    Scripted<3>::install("EndGame", 0x100af040, "83 ec 40 53 55 56 8b f1");
    // All four are needed to tell scripted cinematics apart.
    if (Scripted<0>::orig && Scripted<1>::orig && Scripted<2>::orig && Scripted<3>::orig)
        oUpd = (UpdT)Hook("cinematic update", Dust(0x10096cd0), "83 ec 40 53 55 56 8b f1 33 db", 6, (void*)hUpd);
}

void CutscenesFrame(bool fg) {
    static bool was;
    // Developer option 63, "Skip cinematics".
    if (cfg.autoSkipCinematics && Game())
        if (auto o = At<uintptr_t>(Game() + 0x2bc)) At<int>(o + 63 * 4) = 1;
    if (!CinePlayer()) FastForward(false);      // level unloaded while fast-forwarding
    // Boot logo: the player needs a moment to open.
    if (g_logo && GetTickCount() - g_autoStop > 300 && SkipVideo()) g_logo = false;
    bool down = false;
    if (fg)
        for (int k : cfg.skipKeys)
            if (k && (GetAsyncKeyState(k) & 0x8000)) down = true;
    bool press = down && !was;
    was = down;
    if (!press) return;
    if (cfg.skipVideos && SkipVideo()) return;
    auto p = CinePlayer();
    if (oUpd && p && At<int>(p) >= 0) g_skip = true;
}
