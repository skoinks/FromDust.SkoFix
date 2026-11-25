// Borderless full screen: full screen becomes a window without a frame that covers the monitor.
// Switching to other programs then needs no mode change and no device reset.
#include "common.h"
#include <d3d9.h>

static bool g_active;
static HWND g_window;
static RECT g_monitor;
static LONG g_width, g_height;       // back buffer

typedef LONG(WINAPI* SetLongT)(HWND, int, LONG);
typedef BOOL(WINAPI* SetPosT)(HWND, HWND, int, int, int, int, UINT);
typedef BOOL(WINAPI* CursorPosT)(LPPOINT);
static SetLongT oSetLong;
static SetPosT oSetPos;
static CursorPosT oCursorPos;

static LONG WINAPI hSetLong(HWND w, int index, LONG v) {
    if (g_active && w == g_window) {
        if (index == GWL_STYLE) v = (v & ~(WS_OVERLAPPEDWINDOW | WS_MAXIMIZE)) | WS_POPUP;
        if (index == GWL_EXSTYLE) v &= ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME | WS_EX_STATICEDGE);
    }
    return oSetLong(w, index, v);
}
static BOOL WINAPI hSetPos(HWND w, HWND after, int x, int y, int cx, int cy, UINT flags) {
    if (g_active && w == g_window) {
        x = g_monitor.left;
        y = g_monitor.top;
        cx = g_monitor.right - x;
        cy = g_monitor.bottom - y;
        flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
    }
    return oSetPos(w, after, x, y, cx, cy, flags);
}
// The menus expect the desktop to have the game's resolution.
static BOOL WINAPI hCursorPos(LPPOINT p) {
    BOOL ok = oCursorPos(p);
    LONG w = g_monitor.right - g_monitor.left, h = g_monitor.bottom - g_monitor.top;
    if (ok && g_active && w > 0 && h > 0 && g_width && g_height) {
        p->x = g_monitor.left + MulDiv(p->x - g_monitor.left, g_width, w);
        p->y = g_monitor.top + MulDiv(p->y - g_monitor.top, g_height, h);
    }
    return ok;
}

// The window procedure requests a device reset when the window is restored or loses the focus.
static void ResetOnFocus(bool on) {
    static const struct { uintptr_t va; uint8_t on; } p[] = { { 0x412ecc, 0x74 }, { 0x413073, 0x75 } };
    for (auto& x : p) {
        uint8_t b = on ? x.on : 0xEB;
        WriteMem(x.va, &b, 1);
    }
}

static void Window(const D3DPRESENT_PARAMETERS& p) {
    LONG s = (GetWindowLongA(g_window, GWL_STYLE) & ~WS_POPUP) | WS_OVERLAPPEDWINDOW;
    RECT r = { 0, 0, (LONG)p.BackBufferWidth, (LONG)p.BackBufferHeight };
    AdjustWindowRect(&r, s, FALSE);
    int w = r.right - r.left, h = r.bottom - r.top;
    oSetLong(g_window, GWL_STYLE, s);
    oSetPos(g_window, HWND_NOTOPMOST, (g_monitor.left + g_monitor.right - w) / 2, (g_monitor.top + g_monitor.bottom - h) / 2, w, h, SWP_FRAMECHANGED);
    ResetOnFocus(true);
    Log("[display] window");
}
static void Apply(HWND w, const D3DPRESENT_PARAMETERS& p, HMONITOR monitor) {
    MONITORINFO mi = { sizeof mi };
    bool was = g_active, on = !p.Windowed && GetMonitorInfoA(monitor, &mi);
    g_active = on;
    if (!on) {
        if (was) Window(p);
        return;
    }
    bool same = was && w == g_window && EqualRect(&g_monitor, &mi.rcMonitor);
    g_window = w;
    g_monitor = mi.rcMonitor;
    g_width = p.BackBufferWidth;
    g_height = p.BackBufferHeight;
    if (same) return;
    ResetOnFocus(false);
    hSetLong(w, GWL_STYLE, GetWindowLongA(w, GWL_STYLE));
    hSetLong(w, GWL_EXSTYLE, GetWindowLongA(w, GWL_EXSTYLE));
    hSetPos(w, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_FRAMECHANGED);
    Log("[display] borderless, %ux%u on %ldx%ld", p.BackBufferWidth, p.BackBufferHeight, g_monitor.right - g_monitor.left, g_monitor.bottom - g_monitor.top);
}
static D3DPRESENT_PARAMETERS Borderless(const D3DPRESENT_PARAMETERS& from) {
    auto p = from;
    p.Windowed = TRUE;
    p.FullScreen_RefreshRateInHz = 0;
    p.SwapEffect = D3DSWAPEFFECT_DISCARD;
    p.BackBufferCount = 1;
    if (p.PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE && p.PresentationInterval != D3DPRESENT_INTERVAL_DEFAULT)
        p.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    return p;
}

typedef HRESULT(STDMETHODCALLTYPE* ResetT)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
static ResetT oReset;
static HRESULT STDMETHODCALLTYPE hReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    if (!pp) return oReset(dev, pp);
    D3DDEVICE_CREATION_PARAMETERS c = {};
    IDirect3D9* d3d = nullptr;
    dev->GetCreationParameters(&c);
    if (SUCCEEDED(dev->GetDirect3D(&d3d))) {
        Apply(pp->hDeviceWindow ? pp->hDeviceWindow : c.hFocusWindow, *pp, d3d->GetAdapterMonitor(c.AdapterOrdinal));
        d3d->Release();
    }
    if (!g_active) return oReset(dev, pp);
    auto p = Borderless(*pp);
    return oReset(dev, &p);
}

typedef HRESULT(STDMETHODCALLTYPE* CreateT)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
static CreateT oCreate;
static HRESULT STDMETHODCALLTYPE hCreate(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    if (!pp || type != D3DDEVTYPE_HAL) return oCreate(d3d, adapter, type, focus, flags, pp, out);
    Apply(pp->hDeviceWindow ? pp->hDeviceWindow : focus, *pp, d3d->GetAdapterMonitor(adapter));
    HRESULT r;
    if (g_active) {
        auto p = Borderless(*pp);
        r = oCreate(d3d, adapter, type, focus, flags, &p, out);
        if (FAILED(r)) {
            Log("[display] borderless failed (%08x), using exclusive full screen", (unsigned)r);
            g_active = false;
            Window(*pp);
            return oCreate(d3d, adapter, type, focus, flags, pp, out);
        }
    } else r = oCreate(d3d, adapter, type, focus, flags, pp, out);
    if (FAILED(r) || oReset) return r;
    auto vt = *reinterpret_cast<uintptr_t**>(*out);
    oReset = (ResetT)vt[16];
    uintptr_t to = (uintptr_t)hReset;
    WriteMem((uintptr_t)&vt[16], &to, 4);
    return r;
}

typedef IDirect3D9*(WINAPI* Create9T)(UINT);
static Create9T oCreate9;
static IDirect3D9* WINAPI hCreate9(UINT version) {
    auto d3d = oCreate9(version);
    if (!d3d || oCreate) return d3d;
    auto vt = *reinterpret_cast<uintptr_t**>(d3d);
    oCreate = (CreateT)vt[16];
    uintptr_t to = (uintptr_t)hCreate;
    WriteMem((uintptr_t)&vt[16], &to, 4);
    return d3d;
}

void DisplayInit() {
    if (!IniInt("Graphics", "Borderless", 1)) return;
    if (!BytesMatch(0x412eca, "85 c0 74 0a c7 80 fc 6d 00 00") || !BytesMatch(0x413071, "85 c0 75 54 a1 c0 b4 92 00")) {
        Log("[display] window code differs, skipped");
        return;
    }
    HMODULE exe = GetModuleHandleA(nullptr), user = GetModuleHandleA("user32.dll");
    if (auto aware = (BOOL(WINAPI*)())GetProcAddress(user, "SetProcessDPIAware")) aware();   // real pixels on scaled desktops
    oSetLong = (SetLongT)HookIAT(exe, "USER32.dll", "SetWindowLongA", (void*)hSetLong);
    oSetPos = (SetPosT)HookIAT(exe, "USER32.dll", "SetWindowPos", (void*)hSetPos);
    oCursorPos = (CursorPosT)HookIAT(exe, "USER32.dll", "GetCursorPos", (void*)hCursorPos);
    if (oSetLong && oSetPos && oCursorPos) oCreate9 = (Create9T)HookIAT(exe, "d3d9.dll", "Direct3DCreate9", (void*)hCreate9);
}
