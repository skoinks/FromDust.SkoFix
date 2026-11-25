// Text panel, drawn on the back buffer together with the pointer (pointer.cpp). Filled by the
// gameplay thread, drawn by the presenting thread.
#include "common.h"
#include <d3d9.h>

enum { MAX_LINES = 40 };
static CRITICAL_SECTION g_cs;
static bool g_on;
static PanelLine g_lines[MAX_LINES];
static int g_count;
static volatile DWORD g_shown;
static volatile LONG g_version;

void PanelInit() {
    InitializeCriticalSection(&g_cs);
    g_on = true;
}

void PanelShow(const PanelLine* lines, int n) {
    if (!g_on) return;
    if (n > MAX_LINES) n = MAX_LINES;
    EnterCriticalSection(&g_cs);
    if (n != g_count || memcmp(lines, g_lines, n * sizeof *lines)) {
        memcpy(g_lines, lines, n * sizeof *lines);
        g_count = n;
        g_version++;
    }
    LeaveCriticalSection(&g_cs);
    g_shown = GetTickCount() | 1;
}

void PanelHide() { g_shown = 0; }
bool PanelVisible() { return g_on && g_shown && (int)(GetTickCount() - g_shown) < 500; }

static IDirect3DTexture9* g_tex;
static IDirect3DDevice9* g_texDev;
static int g_texW, g_texH, g_w, g_h;
static LONG g_drawn;
static float g_scale;

// Renders the lines with GDI into the texture.
static bool Render(IDirect3DDevice9* d, float scale) {
    PanelLine lines[MAX_LINES];
    EnterCriticalSection(&g_cs);
    int n = g_count;
    memcpy(lines, g_lines, n * sizeof *lines);
    g_drawn = g_version;
    LeaveCriticalSection(&g_cs);
    static const COLORREF colors[] = { RGB(222, 222, 222), RGB(255, 205, 120), RGB(130, 235, 150), RGB(160, 160, 165) };
    const int line = (int)(24 * scale), pad = (int)(12 * scale), gap = (int)(10 * scale);

    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    HFONT font = CreateFontA(-(int)(16 * scale), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
    auto oldFont = SelectObject(dc, font);
    g_scale = scale;
    int keyW = 0, textW = 0;
    for (int i = 0; i < n; i++) {
        SIZE s;
        if (lines[i].key[0] && GetTextExtentPoint32A(dc, lines[i].key, (int)strlen(lines[i].key), &s) && s.cx > keyW) keyW = s.cx;
    }
    for (int i = 0; i < n; i++) {
        SIZE s;
        int x = lines[i].style == PANEL_HEAD ? 0 : keyW + gap;
        if (GetTextExtentPoint32A(dc, lines[i].text, (int)strlen(lines[i].text), &s) && x + s.cx > textW) textW = x + s.cx;
    }
    g_w = textW + 2 * pad;
    g_h = n * line + 2 * pad;
    if (g_w > 2048) g_w = 2048;
    if (g_h > 2048) g_h = 2048;

    bool ok = false;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = g_w;
    bi.bmiHeader.biHeight = -g_h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits) {
        auto oldBmp = SelectObject(dc, bmp);
        SetBkMode(dc, TRANSPARENT);
        for (int i = 0; i < n; i++) {
            auto& l = lines[i];
            int y = pad + i * line;
            SetTextColor(dc, l.style == PANEL_HEAD ? colors[PANEL_HEAD] : colors[PANEL_DIM]);
            if (l.style != PANEL_HEAD) TextOutA(dc, pad, y, l.key, (int)strlen(l.key));
            SetTextColor(dc, colors[l.style]);
            TextOutA(dc, l.style == PANEL_HEAD ? pad : pad + keyW + gap, y, l.text, (int)strlen(l.text));
        }
        GdiFlush();
        if (!g_tex || g_texDev != d || g_texW < g_w || g_texH < g_h) {
            if (g_tex && g_texDev == d) g_tex->Release();
            g_tex = nullptr;
            g_texDev = d;
            for (g_texW = 64; g_texW < g_w;) g_texW *= 2;
            for (g_texH = 64; g_texH < g_h;) g_texH *= 2;
            if (FAILED(d->CreateTexture(g_texW, g_texH, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_tex, nullptr))) g_tex = nullptr;
        }
        D3DLOCKED_RECT lr;
        if (g_tex && SUCCEEDED(g_tex->LockRect(0, &lr, nullptr, 0))) {
            for (int y = 0; y < g_h; y++) {
                auto src = (DWORD*)bits + y * g_w, dst = (DWORD*)((BYTE*)lr.pBits + y * lr.Pitch);
                for (int x = 0; x < g_w; x++) {
                    DWORD c = src[x] & 0xFFFFFF, r = c >> 16, g = c >> 8 & 255, b = c & 255;
                    DWORD top = r > g ? r : g;
                    if (b > top) top = b;
                    DWORD a = 80 + top * 175 / 222;
                    dst[x] = (a > 255 ? 255 : a) << 24 | c;
                }
            }
            g_tex->UnlockRect(0);
            ok = true;
        }
        SelectObject(dc, oldBmp);
    }
    if (bmp) DeleteObject(bmp);
    SelectObject(dc, oldFont);
    if (font) DeleteObject(font);
    DeleteDC(dc);
    return ok;
}

// At the left edge, with the pointer's render states set.
void PanelDraw(IDirect3DDevice9* d, int width, int height) {
    float scale = height / 1080.0f;
    if (scale < 0.75f) scale = 0.75f;
    if ((!g_tex || g_texDev != d || g_drawn != g_version || g_scale != scale) && !Render(d, scale)) {
        g_on = false;
        Log("[panel] could not be drawn, off");
        return;
    }
    float x = 20 * scale, y = (float)(int)(g_count > 1 ? 24 * scale : height * 0.2f) - 0.5f, u = (float)g_w / g_texW, v = (float)g_h / g_texH;
    x = (float)(int)x - 0.5f;
    struct Vtx { float x, y, z, w; DWORD c; float u, v; } q[4] = {
        { x, y, 0, 1, 0xFFFFFFFF, 0, 0 }, { x + g_w, y, 0, 1, 0xFFFFFFFF, u, 0 },
        { x, y + g_h, 0, 1, 0xFFFFFFFF, 0, v }, { x + g_w, y + g_h, 0, 1, 0xFFFFFFFF, u, v } };
    d->SetTexture(0, g_tex);
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(Vtx));
}
