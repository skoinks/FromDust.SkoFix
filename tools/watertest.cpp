// Test of shaders/water.hlsl: draws a scene with the game's water shaders (Cache/fxlist_PC.fxl) and
// with the plain build, compares them, and saves pictures of the enhanced build.
#include <windows.h>
#include <d3d9.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include <string>
#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "user32.lib")
#include "../src/water_shaders.h"

static const int W = 960, H = 540;
static IDirect3DDevice9* d;
static std::vector<BYTE> Load(const std::string& f) {
    std::vector<BYTE> b;
    FILE* h = fopen(f.c_str(), "rb");
    if (!h) { printf("cannot open %s\n", f.c_str()); exit(1); }
    fseek(h, 0, SEEK_END);
    b.resize(ftell(h));
    fseek(h, 0, SEEK_SET);
    fread(b.data(), 1, b.size(), h);
    fclose(h);
    return b;
}
static DWORD Crc(const BYTE* p, size_t n) {
    DWORD c = ~0u;
    while (n--) { c ^= *p++; for (int i = 0; i < 8; i++) c = c >> 1 ^ (c & 1 ? 0xEDB88320 : 0); }
    return ~c;
}
static std::vector<BYTE> cache;
static const DWORD* Original(size_t at, size_t n, DWORD crc) {
    if (Crc(&cache[at], n) != crc) { printf("shader at %x differs\n", (unsigned)at); exit(1); }
    BYTE* p = new BYTE[n];
    memcpy(p, &cache[at], n);
    return (const DWORD*)p;
}
static IDirect3DPixelShader9* PS(const void* c, const char* n) {
    IDirect3DPixelShader9* s = nullptr;
    HRESULT r = d->CreatePixelShader((const DWORD*)c, &s);
    printf("%-28s %08lx\n", n, r);
    return s;
}
static IDirect3DVertexShader9* VS(const void* c, const char* n) {
    IDirect3DVertexShader9* s = nullptr;
    HRESULT r = d->CreateVertexShader((const DWORD*)c, &s);
    printf("%-28s %08lx\n", n, r);
    return s;
}

// ---- scene --------------------------------------------------------------------------------------
struct V { float x, y, z; };
static V operator+(V a, V b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
static V operator*(V a, float s) { return { a.x * s, a.y * s, a.z * s }; }
static float dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V cross(V a, V b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
static V norm(V a) { return a * (1 / sqrtf(dot(a, a))); }
static V eye = { 10, -170, 55 }, fwd, rgt, up;
static float tx = 0.8f, ty = 0.45f;
static V Ray(float u, float v) { return fwd + rgt * ((u * 2 - 1) * tx) + up * ((1 - v * 2) * ty); }   // view depth 1
struct Hit { float t; V c; };
static Hit Trace(V o, V r) {
    Hit h = { 900, { 0.55f, 0.7f, 0.95f } };
    // sea bed, rising to a beach at the back
    for (float t = 1; t < 900; t += 0.5f) {
        V p = o + r * t;
        float g = -14 + (p.y > 60 ? (p.y - 60) * 0.5f : 0);
        if (g > 45) g = 45;
        if (p.z < g) {
            float k = (int(floorf(p.x / 8)) + int(floorf(p.y / 8))) & 1 ? 1.0f : 0.75f;
            h = { t, { 0.75f * k, 0.6f * k, 0.4f * k } };
            break;
        }
    }
    static const struct { float x, y, s, z; V c; } box[] = { { -50, 0, 8, 40, { 0.9f, 0.15f, 0.1f } }, { 20, 20, 10, 60, { 0.1f, 0.7f, 0.15f } }, { 75, -30, 6, 30, { 0.95f, 0.85f, 0.1f } } };
    for (auto& b : box) {
        float lo[3] = { b.x - b.s, b.y - b.s, -20 }, hi[3] = { b.x + b.s, b.y + b.s, b.z }, oo[3] = { o.x, o.y, o.z }, rr[3] = { r.x, r.y, r.z }, a = 0, z = 1e9f;
        for (int i = 0; i < 3; i++) {
            float t0 = (lo[i] - oo[i]) / rr[i], t1 = (hi[i] - oo[i]) / rr[i];
            if (t0 > t1) { float q = t0; t0 = t1; t1 = q; }
            if (t0 > a) a = t0;
            if (t1 < z) z = t1;
        }
        if (a < z && a > 0.1f && a < h.t) h = { a, b.c };
    }
    return h;
}

// ---- textures -----------------------------------------------------------------------------------
static IDirect3DTexture9* Float(int w, int h, const std::vector<float>& f) {
    IDirect3DTexture9* t = nullptr;
    d->CreateTexture(w, h, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_MANAGED, &t, nullptr);
    D3DLOCKED_RECT l;
    t->LockRect(0, &l, nullptr, 0);
    for (int y = 0; y < h; y++) memcpy((BYTE*)l.pBits + y * l.Pitch, &f[y * w * 4], w * 16);
    t->UnlockRect(0);
    return t;
}
static float Rnd() { return rand() / (float)RAND_MAX; }
static std::vector<float> Noise(int n, int cells, float lo, float hi) {     // smooth, wraps
    std::vector<float> g(cells * cells * 4), o(n * n * 4);
    for (auto& x : g) x = lo + (hi - lo) * Rnd();
    for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) {
        float fx = x * (float)cells / n, fy = y * (float)cells / n;
        int ix = (int)fx, iy = (int)fy;
        fx -= ix; fy -= iy;
        fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
        for (int c = 0; c < 4; c++) {
            auto G = [&](int i, int j) { return g[((j % cells) * cells + i % cells) * 4 + c]; };
            o[(y * n + x) * 4 + c] = (G(ix, iy) * (1 - fx) + G(ix + 1, iy) * fx) * (1 - fy) + (G(ix, iy + 1) * (1 - fx) + G(ix + 1, iy + 1) * fx) * fy;
        }
    }
    return o;
}
static void Set(int s, IDirect3DBaseTexture9* t, bool linear, bool wrap) {
    d->SetTexture(s, t);
    d->SetSamplerState(s, D3DSAMP_MINFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    d->SetSamplerState(s, D3DSAMP_MAGFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    d->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    d->SetSamplerState(s, D3DSAMP_ADDRESSU, wrap ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP);
    d->SetSamplerState(s, D3DSAMP_ADDRESSV, wrap ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP);
}

static IDirect3DSurface9 *rt, *sys;
static std::vector<float> Read() {
    d->GetRenderTargetData(rt, sys);
    D3DLOCKED_RECT l;
    sys->LockRect(&l, nullptr, D3DLOCK_READONLY);
    std::vector<float> o(W * H * 4);
    for (int y = 0; y < H; y++) memcpy(&o[y * W * 4], (BYTE*)l.pBits + y * l.Pitch, W * 16);
    sys->UnlockRect();
    return o;
}
static void Save(const char* name, const std::vector<float>& f, const std::vector<float>& scene, const std::vector<float>& wz) {
    std::vector<BYTE> b(54 + W * H * 3 + H * ((4 - W * 3 % 4) % 4));
    BITMAPFILEHEADER fh = { 0x4d42, (DWORD)b.size(), 0, 0, 54 };
    BITMAPINFOHEADER ih = { 40, W, H, 1, 24 };
    memcpy(&b[0], &fh, 14);
    memcpy(&b[14], &ih, 40);
    int pitch = (W * 3 + 3) & ~3;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = (y * W + x) * 4;
        float a = wz[i] < 990 ? f[i + 3] : 0;
        for (int c = 0; c < 3; c++) {
            float v = scene[i + c] * (1 - a) + f[i + c] * a;
            b[54 + (H - 1 - y) * pitch + x * 3 + 2 - c] = (BYTE)(255 * (v < 0 ? 0 : v > 1 ? 1 : v));
        }
    }
    FILE* h = fopen(name, "wb");
    fwrite(b.data(), 1, b.size(), h);
    fclose(h);
}

// ---- drawing -------------------------------------------------------------------------------------
struct QV { float x, y, z, u, v; };
static IDirect3DVertexDeclaration9 *dq, *dp;
static std::vector<float> DrawDeferred(IDirect3DVertexShader9* v, IDirect3DPixelShader9* p) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1, 0);
    d->BeginScene();
    d->SetVertexDeclaration(dq);
    d->SetVertexShader(v);
    d->SetPixelShader(p);
    float o = 0.5f;
    QV q[4];
    for (int i = 0; i < 4; i++) {           // positions shifted by half a pixel, as d3d9 wants
        float x = (float)(i & 1), y = (float)(i >> 1);
        q[i] = { x - o / W, y - o / H, 0, x, y };
    }
    HRESULT r = d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(QV));
    d->EndScene();
    if (r) printf("draw %08lx\n", r);
    return Read();
}
static std::vector<float> DrawForward(IDirect3DVertexShader9* v, IDirect3DPixelShader9* p) {
    d->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1, 0);
    d->BeginScene();
    d->SetVertexDeclaration(dp);
    d->SetVertexShader(v);
    d->SetPixelShader(p);
    float q[4][3] = { { -250, -250, 0 }, { 250, -250, 0 }, { -250, 250, 0 }, { 250, 250, 0 } };
    HRESULT r = d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, 12);
    d->EndScene();
    if (r) printf("draw %08lx\n", r);
    return Read();
}
static void Compare(const char* n, const std::vector<float>& a, const std::vector<float>& b, const std::vector<float>& wz) {
    double sum = 0, worst = 0;
    long cnt = 0, over = 0, drawn = 0;
    int at = 0;
    for (int i = 0; i < W * H; i++) {
        if (wz[i * 4] >= 990) continue;
        for (int c = 0; c < 4; c++) {
            double e = fabs(a[i * 4 + c] - b[i * 4 + c]);
            if (!(e < 1e9)) e = 9;
            sum += e;
            cnt++;
            if (e > 1.0 / 255) over++;
            if (e > worst) { worst = e; at = i; }
        }
        if (a[i * 4] != 0) drawn++;
    }
    printf("%s: %ld water pixels drawn, average difference %.6f, worst %.4f at %d,%d, more than 1/255: %.3f%%\n", n, drawn, sum / cnt, worst, at % W, at / W, 100.0 * over / cnt);
}

int main(int argc, char** argv) {
    std::string root = argc > 1 ? argv[1] : "..", build = root + "\\build\\";
    cache = Load(root + "\\..\\Cache\\fxlist_PC.fxl");
    HWND w = CreateWindowA("STATIC", "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
    auto d9 = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = pp.BackBufferHeight = 64;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    if (FAILED(d9->CreateDevice(0, D3DDEVTYPE_HAL, w, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &d))) { printf("no device\n"); return 1; }
    d->CreateRenderTarget(W, H, D3DFMT_A32B32G32R32F, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr);
    d->CreateOffscreenPlainSurface(W, H, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM, &sys, nullptr);
    d->SetRenderTarget(0, rt);
    d->SetDepthStencilSurface(nullptr);
    d->SetRenderState(D3DRS_ZENABLE, 0);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, 0);
    D3DVERTEXELEMENT9 eq[] = { { 0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0 }, { 0, 12, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 0 }, D3DDECL_END() };
    D3DVERTEXELEMENT9 ep[] = { { 0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0 }, D3DDECL_END() };
    d->CreateVertexDeclaration(eq, &dq);
    d->CreateVertexDeclaration(ep, &dp);

    auto oVS = VS(Original(0x13126, 820, 0x1a257034), "game deferred vs");
    auto oPS = PS(Original(0x13476, 2980, 0x53f7b68d), "game deferred ps");
    auto fVS = VS(Original(0x17b6, 804, 0xfd66be4e), "game forward vs");
    auto fPS = PS(Original(0x1af6, 2784, 0x4ade5a84), "game forward ps");
    auto pD = PS(Load(build + "g_plainDeferred.cso").data(), "plain deferred");
    auto pF = PS(Load(build + "g_plainForward.cso").data(), "plain forward");
    auto eD = PS(g_waterDeferred, "enhanced deferred");
    auto eF = PS(g_waterForward, "enhanced forward");
    auto eV = VS(g_waterForwardVS, "enhanced forward vs");
    if (!oVS || !oPS || !fVS || !fPS || !pD || !pF || !eD || !eF || !eV) return 1;

    // camera and pictures of the scene
    fwd = norm(V{ 0, 0, -5 } + eye * -1);
    rgt = norm(cross(fwd, { 0, 0, 1 }));
    up = cross(rgt, fwd);
    std::vector<float> scene(W * H * 4), depth(W * H * 4), wz(W * H * 4, 1000);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        V r = Ray((x + 0.5f) / W, (y + 0.5f) / H);
        Hit h = Trace(eye, r);
        int i = (y * W + x) * 4;
        scene[i] = h.c.x; scene[i + 1] = h.c.y; scene[i + 2] = h.c.z;
        depth[i + 3] = h.t;
        float t = r.z < 0 ? -eye.z / r.z : 1e9f;
        if (t < h.t - 0.4f && t < 900) wz[i] = t;
    }
    srand(5);
    auto noise = [](int n, int c, float lo, float hi, bool) { return Float(n, n, Noise(n, c, lo, hi)); };
    std::vector<float> scroll(512 * 512 * 4), nz = Noise(512, 24, -0.01f, 0.01f);
    for (int y = 0; y < 512; y++) for (int x = 0; x < 512; x++) for (int c = 0; c < 4; c++) scroll[(y * 512 + x) * 4 + c] = ((c & 1 ? y : x) + 0.5f) / 512 + nz[(y * 512 + x) * 4 + c];
    auto params = Noise(512, 40, -1.5f, 1.0f);
    for (int i = 0; i < 512 * 512; i++) { float* p = &params[i * 4]; p[2] = p[2] < 0 ? 0 : p[2]; p[3] = p[3] < 0.4f ? 0 : p[3] - 0.4f; }
    std::vector<float> ramp(64 * 64 * 4);
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
        float* p = &ramp[(y * 64 + x) * 4], l = x / 63.0f, deep = 1 - y / 63.0f;
        p[0] = 0.02f * l; p[1] = 0.25f * l; p[2] = 0.4f * l; p[3] = deep;
    }
    auto ripple = Noise(256, 64, 0, 1);
    for (int i = 0; i < 256 * 256; i++) { float* p = &ripple[i * 4]; p[0] = 0.25f + (p[0] - 0.5f) * 0.4f; p[1] = 0.25f + (p[1] - 0.5f) * 0.4f; }
    IDirect3DCubeTexture9* cube = nullptr;
    d->CreateCubeTexture(16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cube, nullptr);
    for (int f = 0; f < 6; f++) {
        D3DLOCKED_RECT l;
        cube->LockRect((D3DCUBEMAP_FACES)f, 0, &l, nullptr, 0);
        for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) ((DWORD*)((BYTE*)l.pBits + y * l.Pitch))[x] = 0xff203040 + f * 0x080808 + x * 0x020100;
        cube->UnlockRect((D3DCUBEMAP_FACES)f, 0);
    }
    Set(0, Float(512, 512, params), true, false);
    Set(1, Float(512, 512, scroll), false, false);
    Set(3, Float(64, 64, ramp), true, false);
    Set(4, noise(512, 80, 0.4f, 0.6f, false), true, false);
    Set(5, Float(W, H, wz), false, false);
    Set(6, noise(512, 30, 0.1f, 1, false), true, false);
    Set(8, cube, true, false);
    Set(9, Float(256, 256, ripple), true, true);
    Set(10, Float(W, H, scene), true, false);
    Set(14, noise(64, 6, 0, 0.3f, false), true, true);
    Set(15, Float(W, H, depth), false, false);

    // constants
    V s = norm({ 0.15f, 0.8f, 0.35f });
    float sun[4] = { s.x, s.y, s.z, 0 };
    float add[4] = { 256, 256, 1 / 512.0f, 1 / 512.0f }, view[4] = { eye.x, eye.y, eye.z, 1 };
    float proj[4] = { 2 * tx, -2 * ty, -tx, ty };                // ray.xy from place on screen
    float inv[12] = { rgt.x, up.x, fwd.x, eye.x, rgt.y, up.y, fwd.y, eye.y, rgt.z, up.z, fwd.z, eye.z };
    float c36[4] = { 0.35f, 0, 0, 0 }, c37[4] = { 0.5f, 0.35f, 0.2f, 1 }, c38[4] = { 0, 0, 24, 1.2f }, c40[4] = { 0, 0, 1.5f, 0 }, c42[4] = { 0, 0, 0.005f, 20 };
    float c65[8] = { 0.3f, 0.2f, 0, 0, 0.1f, 0.2f, 3, 3 }, c64[4] = {};
    d->SetPixelShaderConstantF(36, c36, 1);
    d->SetPixelShaderConstantF(37, c37, 1);
    d->SetPixelShaderConstantF(38, c38, 1);
    d->SetPixelShaderConstantF(40, c40, 1);
    d->SetPixelShaderConstantF(42, c42, 1);
    d->SetPixelShaderConstantF(50, inv, 3);
    d->SetPixelShaderConstantF(65, c65, 2);
    d->SetVertexShaderConstantF(33, sun, 1);
    d->SetVertexShaderConstantF(34, add, 1);
    d->SetVertexShaderConstantF(36, view, 1);
    d->SetVertexShaderConstantF(50, proj, 1);
    d->SetVertexShaderConstantF(64, c64, 1);

    auto a = DrawDeferred(oVS, oPS), b = DrawDeferred(oVS, pD);
    Compare("deferred, game and plain", a, b, wz);
    Save("water-game.bmp", a, scene, wz);
    Save("water-enhanced.bmp", DrawDeferred(oVS, eD), scene, wz);

    // forward: view projection rows, from world to clip space
    float hx = 1.0f / W, hy = 1.0f / H, vp[16];
    V ax[3] = { rgt * (1 / tx), up * (1 / ty), fwd };
    float off[3];
    for (int i = 0; i < 3; i++) off[i] = -dot(ax[i], eye);
    float rows[3][4] = { { ax[0].x, ax[0].y, ax[0].z, off[0] }, { ax[1].x, ax[1].y, ax[1].z, off[1] }, { fwd.x, fwd.y, fwd.z, off[2] } };
    for (int i = 0; i < 4; i++) {
        vp[i] = rows[0][i] - hx * rows[2][i];                     // half a pixel
        vp[4 + i] = rows[1][i] + hy * rows[2][i];
        vp[8 + i] = rows[2][i] * 0.5f;                            // any depth inside
        vp[12 + i] = rows[2][i];
    }
    float p2t[4] = { 0.5f, -0.5f, 0.5f + 0.5f * hx, 0.5f + 0.5f * hy };
    d->SetVertexShaderConstantF(24, vp, 4);
    d->SetVertexShaderConstantF(35, p2t, 1);
    a = DrawForward(fVS, fPS);
    b = DrawForward(eV, pF);
    Compare("forward, game and plain", a, b, wz);
    Save("water-forward-game.bmp", a, scene, wz);
    Save("water-forward-enhanced.bmp", DrawForward(eV, eF), scene, wz);
    // what it costs
    IDirect3DQuery9 *q0, *q1, *qd, *qf;
    d->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &q0);
    d->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &q1);
    d->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &qd);
    d->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &qf);
    IDirect3DPixelShader9* two[2] = { oPS, eD };
    for (int k = 0; k < 2; k++) {
        qd->Issue(D3DISSUE_BEGIN);
        q0->Issue(D3DISSUE_END);
        for (int i = 0; i < 50; i++) {
            d->BeginScene();
            d->SetVertexDeclaration(dq);
            d->SetVertexShader(oVS);
            d->SetPixelShader(two[k]);
            QV q[4] = { { 0, 0, 0, 0, 0 }, { 1, 0, 0, 1, 0 }, { 0, 1, 0, 0, 1 }, { 1, 1, 0, 1, 1 } };
            d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(QV));
            d->EndScene();
        }
        q1->Issue(D3DISSUE_END);
        qf->Issue(D3DISSUE_END);
        qd->Issue(D3DISSUE_END);
        UINT64 t0 = 0, t1 = 0, f = 1;
        while (q1->GetData(&t1, 8, D3DGETDATA_FLUSH) == S_FALSE) Sleep(0);
        while (q0->GetData(&t0, 8, D3DGETDATA_FLUSH) == S_FALSE) Sleep(0);
        while (qf->GetData(&f, 8, D3DGETDATA_FLUSH) == S_FALSE) Sleep(0);
        printf("%s: %.3f ms for %dx%d\n", k ? "enhanced" : "game", 1000.0 * (t1 - t0) / f / 50, W, H);
    }
    return 0;
}
