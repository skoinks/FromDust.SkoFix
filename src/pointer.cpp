// Ordinary mouse pointer, drawn on the back buffer before Present. Drawn by the presenting thread,
// positioned by the gameplay thread.
#include "common.h"
#include <d3d9.h>
#include <math.h>

static volatile float g_x, g_y;      // 0..1, y down
static volatile DWORD g_tick;        // time of the last PointerShow
static volatile bool g_any;
static int g_size;
static volatile bool g_broken;
static volatile bool g_off;
void PointerToggle() {
    g_off = !g_off;
    Log("[pointer] %s", g_off ? "off" : "on");
}

static bool g_installed;
bool PointerReady() { return g_installed && !g_broken && !g_off; }
void PointerShow(float x, float y) {
    g_x = x;
    g_y = y;
    g_tick = GetTickCount();
    g_any = true;
}

// ---- image: a drop with its tip at the top left, 4x4 supersampled -------------------------
enum { TEX = 64 };
static bool InDrop(float x, float y, float grow) {
    const float cx = 34, cy = 40, r = 18 + grow;            // round part
    float dx = x - cx, dy = y - cy;
    if (dx * dx + dy * dy <= r * r) return true;
    // Cone from the tip to the round part.
    const float tx = 5 - grow * 0.8f, ty = 4 - grow * 0.9f;
    float ax = cx - tx, ay = cy - ty, len = sqrtf(ax * ax + ay * ay);
    float along = ((x - tx) * ax + (y - ty) * ay) / len, side = fabsf((x - tx) * ay - (y - ty) * ax) / len;
    float sine = r / len;
    return along > 0 && along < len * (1 - sine * sine) + 0.5f && side <= along * sine / sqrtf(1 - sine * sine);
}
static IDirect3DTexture9* g_tex;
static IDirect3DDevice9* g_texDev;
static IDirect3DTexture9* Texture(IDirect3DDevice9* d) {
    if (g_tex && g_texDev == d) return g_tex;
    g_tex = nullptr;
    g_texDev = d;
    if (FAILED(d->CreateTexture(TEX, TEX, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_tex, nullptr))) { g_broken = true; return nullptr; }
    D3DLOCKED_RECT lr;
    if (FAILED(g_tex->LockRect(0, &lr, nullptr, 0))) return nullptr;
    for (int y = 0; y < TEX; y++)
        for (int x = 0; x < TEX; x++) {
            int edge = 0, fill = 0, dot = 0;
            for (int s = 0; s < 16; s++) {
                float px = x + (s % 4 + 0.5f) / 4, py = y + (s / 4 + 0.5f) / 4;
                if (InDrop(px, py, 3)) edge++;
                if (InDrop(px, py, 0)) fill++;
                if ((px - 34) * (px - 34) + (py - 40) * (py - 40) < 49) dot++;
            }
            int white = fill > dot ? fill - dot : 0;
            DWORD a = edge * 255 / 16, c = edge ? (30 * (edge - white) + 245 * white) / edge : 0;
            ((DWORD*)((BYTE*)lr.pBits + y * lr.Pitch))[x] = a << 24 | c << 16 | c << 8 | c;
        }
    g_tex->UnlockRect(0);
    return g_tex;
}

static void Draw(IDirect3DDevice9* d) {
    IDirect3DSurface9 *bb = nullptr, *rt = nullptr, *ds = nullptr;
    IDirect3DStateBlock9* sb = nullptr;
    auto tex = Texture(d);
    if (!tex || FAILED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    D3DSURFACE_DESC desc;
    bb->GetDesc(&desc);
    if (SUCCEEDED(d->CreateStateBlock(D3DSBT_ALL, &sb))) {
        d->GetRenderTarget(0, &rt);
        d->GetDepthStencilSurface(&ds);
        d->SetRenderTarget(0, bb);                       // also sets a full viewport
        d->SetDepthStencilSurface(nullptr);
        bool began = SUCCEEDED(d->BeginScene());         // fails if the engine left its scene open
        for (int i = 1; i < 4; i++) d->SetTexture(i, nullptr);
        d->SetVertexShader(nullptr);
        d->SetPixelShader(nullptr);
        d->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        d->SetTexture(0, tex);
        d->SetStreamSourceFreq(0, 1);                // in case instancing was left on
        d->SetStreamSourceFreq(1, 1);
        static const DWORD rs[][2] = {
            { D3DRS_ZENABLE, 0 }, { D3DRS_ZWRITEENABLE, 0 }, { D3DRS_ALPHATESTENABLE, 0 }, { D3DRS_CULLMODE, D3DCULL_NONE },
            { D3DRS_ALPHABLENDENABLE, 1 }, { D3DRS_SRCBLEND, D3DBLEND_SRCALPHA }, { D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA },
            { D3DRS_BLENDOP, D3DBLENDOP_ADD }, { D3DRS_SEPARATEALPHABLENDENABLE, 0 }, { D3DRS_STENCILENABLE, 0 },
            { D3DRS_SCISSORTESTENABLE, 0 }, { D3DRS_FOGENABLE, 0 }, { D3DRS_LIGHTING, 0 }, { D3DRS_SRGBWRITEENABLE, 0 },
            { D3DRS_COLORWRITEENABLE, 0xF }, { D3DRS_MULTISAMPLEMASK, 0xFFFFFFFF }, { D3DRS_ADAPTIVETESS_Y, 0 },
            { D3DRS_FILLMODE, D3DFILL_SOLID }, { D3DRS_CLIPPLANEENABLE, 0 } };
        for (auto& r : rs) d->SetRenderState((D3DRENDERSTATETYPE)r[0], r[1]);
        static const DWORD ts[][2] = {
            { D3DTSS_COLOROP, D3DTOP_SELECTARG1 }, { D3DTSS_COLORARG1, D3DTA_TEXTURE }, { D3DTSS_ALPHAOP, D3DTOP_SELECTARG1 },
            { D3DTSS_ALPHAARG1, D3DTA_TEXTURE }, { D3DTSS_TEXCOORDINDEX, 0 }, { D3DTSS_TEXTURETRANSFORMFLAGS, 0 } };
        for (auto& t : ts) d->SetTextureStageState(0, (D3DTEXTURESTAGESTATETYPE)t[0], t[1]);
        d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        d->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        d->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, 0);
        float s = g_size * desc.Height / 1080.0f, k = s / TEX;
        float x = floorf(g_x * desc.Width - 5 * k) - 0.5f, y = floorf(g_y * desc.Height - 4 * k) - 0.5f;   // hot spot: the tip
        struct Vtx { float x, y, z, w; DWORD c; float u, v; } q[4] = {
            { x, y, 0, 1, 0xFFFFFFFF, 0, 0 }, { x + s, y, 0, 1, 0xFFFFFFFF, 1, 0 },
            { x, y + s, 0, 1, 0xFFFFFFFF, 0, 1 }, { x + s, y + s, 0, 1, 0xFFFFFFFF, 1, 1 } };
        d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(Vtx));
        if (began) d->EndScene();
        if (rt) { d->SetRenderTarget(0, rt); rt->Release(); }
        d->SetDepthStencilSurface(ds);
        if (ds) ds->Release();
        sb->Apply();                                     // everything else, viewport included
        sb->Release();
    }
    bb->Release();
}

typedef void(__fastcall* PreT)(uintptr_t, void*);
static PreT oPre;
static void __fastcall hPre(uintptr_t gfx, void* edx) {
    oPre(gfx, edx);
    auto dev = At<IDirect3DDevice9*>(gfx + 0x2710);
    if (g_broken || g_off || !dev || !g_any || (int)(GetTickCount() - g_tick) > 100) return;   // signed, g_tick may be newer
    __try {
        // The engine's device lock, shared with its loading threads.
        auto cs = (CRITICAL_SECTION*)0x92b500;
        if (!TryEnterCriticalSection(cs)) return;
        __try {
            if (dev->TestCooperativeLevel() == D3D_OK) Draw(dev);
        } __finally {
            LeaveCriticalSection(cs);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_broken = true;
        Log("[pointer] drawing raised %08x, pointer off", GetExceptionCode());
    }
}

void PointerInit() {
    if (!IniInt("Mouse", "NormalCursor", 1)) return;
    g_size = IniInt("Mouse", "NormalCursorSize", 40);
    if (!BytesMatch(0x53b190, "83 ec 10 53 56 8b f1 e8 b4 f7 00 00 bb 01 00 00 00")) { Log("[pointer] code differs, skipped"); return; }
    oPre = (PreT)0x54a950;
    int32_t rel = (int32_t)((uintptr_t)hPre - (0x53b197 + 5));
    WriteMem(0x53b198, &rel, 4);
    g_installed = true;
    Log("[pointer] normal cursor ok");
}
