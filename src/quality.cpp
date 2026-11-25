// Level of detail and sun shadows.
#include "common.h"

static float g_dist;
static int g_all;

// ---- sun shadows ---------------------------------------------------------------------------
// Sun settings: +0 cascade count, +4 distance, +8 split factor, +c smallest caster, +70 split
// factor per cascade. Each cascade ends at the next one's end / (factor * its split). Scaling the
// first split with the distance keeps the near cascades and stretches the last.
typedef void(__fastcall* SunT)(uintptr_t, void*, void*);
static SunT oSun;
static uintptr_t Sun(uintptr_t self) {               // as in 45cd90
    auto light = At<uintptr_t>(self + 0x88);
    auto sun = light ? At<uintptr_t>(light + 0x90) : 0;
    auto world = At<uintptr_t>(At<uintptr_t>(0x921614) + 0x676b0);
    if (world && (At<int>(world + 0xc) & 0x80000) && (At<int>(world + 0x4d0) & 1)) sun = world + 0x428;
    return sun;
}
static void __fastcall hSun(uintptr_t self, void* edx, void* view) {
    auto sun = Sun(self);
    if (!sun) { oSun(self, edx, view); return; }
    float d = At<float>(sun + 4), th = At<float>(sun + 0xc), vp = At<float>(sun + 0x70);
    if (g_dist > d && d > 1) {
        At<float>(sun + 4) = g_dist;
        At<float>(sun + 0x70) = vp * g_dist / d;
    }
    if (g_all) At<float>(sun + 0xc) = 0;
    oSun(self, edx, view);
    At<float>(sun + 4) = d;
    At<float>(sun + 0xc) = th;
    At<float>(sun + 0x70) = vp;
}

// The job that selects terrain tiles and plants (UpdateDisplayList) uses the cascade matrices of
// the last frame drawn (viewer+3084, 64 bytes each), so casters outside the box go missing while
// the camera turns. Give the job a wider and deeper box.
typedef void(__fastcall* ViewT)(uintptr_t, void*, int);
static ViewT oView;
static void __fastcall hView(uintptr_t self, void* edx, int pass) {
    oView(self, edx, pass);
    if (pass != 3) return;
    auto shadow = At<uintptr_t>(At<uintptr_t>(0x921760) + 0x27b8);      // as 4567a0
    auto map = shadow ? At<uintptr_t>(shadow + 0xa4) : 0;
    unsigned n = map ? At<unsigned>(map + 0x8c) >> 8 & 0xf : 0;
    auto base = At<uintptr_t>(self + 0x3084);
    if (!base || n > 3) return;
    auto m = reinterpret_cast<float*>(base + n * 64);
    for (int r = 0; r < 4; r++) {
        m[r * 4] *= 0.2f;
        m[r * 4 + 1] *= 0.2f;
        m[r * 4 + 2] *= 0.05f;
    }
    m[14] += 0.5f * (1 - 0.05f);                                        // around its middle
}

// Sets the engine's unused option for double size shadow maps (renderer+6766c) where the renderer
// is constructed. Replaces mov [esi+6766c], ebx.
static __declspec(naked) void HiresStub() {
    __asm {
        mov dword ptr [esi + 0x6766c], 1
        ret
    }
}
static bool Hires() {
    if (!BytesMatch(0x417be0, "89 9e 6c 76 06 00 89 9e 94 03 00 00")) { Log("[shadow] renderer code differs, skipped"); return false; }
    uint8_t b[6] = { 0xE8, 0, 0, 0, 0, 0x90 };
    *(int32_t*)(b + 1) = (int32_t)((uintptr_t)HiresStub - 0x417be5);
    return WriteMem(0x417be0, b, 6);
}
// Doubles the sun's base size and the colour target that has to match it.
static void Hires4() {
    static const struct { uintptr_t va; const char *from, *to; } p[] = {
        { 0x45b6f9, "b9 00 08 00 00 b8 00 02 00 00", "b9 00 10 00 00 b8 00 04 00 00" },
        { 0x45b4d9, "c1 e3 09 3b cb", "c1 e3 0a" },
        { 0x45b4ed, "c1 e2 0b 3b ca", "c1 e2 0c" },
        { 0x45b520, "25 00 fa ff ff 05 00 08 00 00", "25 00 f4 ff ff 05 00 10 00 00" },
        { 0x45b544, "68 00 02 00 00 e8 c2 fd ff ff", "68 00 04 00 00" },
    };
    for (auto& x : p) if (!BytesMatch(x.va, x.from)) { Log("[shadow] %08x differs, staying at double resolution", (unsigned)x.va); return; }
    for (auto& x : p) Patch("sun shadow size", x.va, x.from, x.to);
}

// ---- plants --------------------------------------------------------------------------------
// Level data: 16 types of 4 growth stages; visuals [view+2edc][detail][type][stage], distances
// [view+2ee0][type][4]. Beyond distance n the job (UpdateVegetation) uses model n + 1, beyond the
// third a sprite, beyond the fourth nothing. 444eb0 passes them to the job: hand over a single
// far distance instead, starting at the best model a type has.
typedef void(__fastcall* PlantsT)(uintptr_t, void*, uintptr_t, int, int);
static PlantsT oPlants;
static void __fastcall hPlants(uintptr_t self, void* edx, uintptr_t view, int a, int b) {
    auto vis = At<uintptr_t*>(view + 0x2edc);
    auto dist = At<float*>(view + 0x2ee0);
    if (!vis || !dist) { oPlants(self, edx, view, a, b); return; }
    float keep[64];
    memcpy(keep, dist, sizeof keep);
    for (int t = 0; t < 16; t++) {
        unsigned have[3] = {};
        for (int l = 0; l < 3; l++) for (int v = 0; v < 4; v++) have[l] |= (vis[l * 64 + t * 4 + v] != 0) << v;
        int best = 0;
        while (best < 3 && !have[best]) best++;
        if (best < 3) for (int i = best; i < 4; i++) dist[t * 4 + i] = 60000;
    }
    oPlants(self, edx, view, a, b);
    memcpy(dist, keep, sizeof keep);
}
// Flags per type and stage (viewer+380, b0 bytes each, +28): 100..800 shadow cascades,
// 1000/2000/4000 shadows of the three models.
typedef void(__fastcall* PlantFlagsT)(uintptr_t);
static PlantFlagsT oPlantFlags;
static void __fastcall hPlantFlags(uintptr_t self) {
    oPlantFlags(self);
    for (int i = 0; i < 64; i++) {
        auto& f = At<unsigned>(self + 0x380 + i * 0xb0 + 0x28);
        if ((f & 0xf00) && (f & 0x7000)) f |= 0x7f00;
    }
}

void QualityInit() {
    g_dist = IniFloat("Graphics", "ShadowDistance", 1200);
    g_all = IniInt("Graphics", "ShadowAllCasters", 1);
    int res = IniInt("Graphics", "ShadowResolution", 4);
    if (res >= 2 && Hires() && res >= 4) Hires4();
    oSun = (SunT)Hook("sun shadow", 0x45cd80, "55 8b ec 83 e4 f0 81 ec 14 05 00 00", 6, (void*)hSun);
    if (g_all) oView = (ViewT)Hook("shadow view", 0x41d870, "55 8b ec 83 e4 f0 8b 45 08", 6, (void*)hView);
    if (g_all) oPlantFlags = (PlantFlagsT)Hook("plant shadows", 0x4538e0, "55 8b ec 83 e4 f8 83 ec 30 8b 01", 6, (void*)hPlantFlags);
    if (!IniInt("Graphics", "BestModels", 1)) return;
    oPlants = (PlantsT)Hook("plant models", 0x444eb0, "55 8b ec 83 e4 f0 a1 34 15 92 00", 6, (void*)hPlants);
    // Terrain tiles: 11.31 / distance above the second value takes the best model, above the first
    // the middle one. Eight copies of (0.1, 0.2, 11.31, 0.5).
    for (uintptr_t a = 0x89d3a0; a < 0x89d420; a += 16) {
        static const float none[2] = { 0, 0 };
        if (!BytesMatch(a, "cd cc cc 3d cd cc 4c 3e f3 04 35 41 00 00 00 3f") || !WriteMem(a, none, 8)) { Log("[quality] tile detail at %08x differs, skipped", (unsigned)a); break; }
        if (a == 0x89d410) Log("[quality] tile detail ok");
    }
    // Objects (4b8c70): leave the loop over the models at the first, also in shadow passes.
    Patch("object model", 0x4b8cc4, "74 12 f3 0f 10 08 0f 2f c8", "eb 12");
    Patch("object model in shadows", 0x4b8ce5, "74 0d 83 fb 05 73 08", "eb 0d");
}
void QualityDustLoaded() {
    if (!IniInt("Graphics", "BestModels", 1)) return;
    // Villagers: level 0 instead of the level chosen by distance (100ce580).
    Patch("villager model", Dust(0x100ce648), "bb 03 00 00 00 0f 28 d0", "33 db eb 42 90");
}
