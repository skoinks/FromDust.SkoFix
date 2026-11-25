#pragma once
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Config {
    int offline = 1, debug = 0;
    int skipVideos = 1, skipCinematics = 1, autoSkipCinematics = 0;
    int skipKeys[8] = {};
};
extern Config cfg;
extern char g_dir[MAX_PATH];

void Log(const char* fmt, ...);
bool Foreground();                                              // the game has the focus
int IniInt(const char* section, const char* key, int def);
int IniKeys(const char* section, const char* key, const char* def, int* out, int max);   // "Space, F5" to key codes
float IniFloat(const char* section, const char* key, float def);

// ---- patching ---------------------------------------------------------------
bool BytesMatch(uintptr_t va, const char* hexPattern);          // "55 8b ?? ec"
bool WriteMem(uintptr_t va, const void* src, size_t n);
// Writes `bytes` if the current bytes match `expect`.
bool Patch(const char* name, uintptr_t va, const char* expect, const char* bytes);
// Inline hook over `n` bytes (whole, position independent instructions). Returns a trampoline,
// or null if the current bytes do not match `expect`.
void* Hook(const char* name, uintptr_t va, const char* expect, int n, void* to);
// Import hook on a module; returns the previous pointer.
void* HookIAT(HMODULE mod, const char* dll, const char* fn, void* to);

template <class T> inline T& At(uintptr_t a) { return *reinterpret_cast<T*>(a); }

// ---- gameplay DLL (dustai.win32.f.dll), relocatable ---------------------------
extern uintptr_t g_dust;                                          // actual base or 0
inline uintptr_t Dust(uintptr_t va) { return g_dust + (va - 0x10000000); }
inline uintptr_t Game() { return g_dust ? At<uintptr_t>(Dust(0x1017a89c)) : 0; }

// ---- features -----------------------------------------------------------------
void CutscenesInit();
void CutscenesFrame(bool foreground);
void CutscenesDustLoaded();
void MouseInit();
void MouseFrame();
void MousePreFrame();
void CursorInit();
void PointerInit();
void PointerToggle();
bool PointerReady();
void PointerShow(float x, float y);
void CursorDustLoaded();
extern volatile int g_curOver;
extern volatile float g_curDx, g_curDy;
void MouseDustLoaded();
void FrameInit();
void RatesInit();
void HangInit();
void QualityInit();
void QualityDustLoaded();
void WaterInit();
void DisplayInit();
void JobsInit(uintptr_t kernel);
void JobsUnloaded();
void JobStats(char* out, int size);
extern volatile LONG g_beat;
extern const char* volatile g_doing;
void RatesDustLoaded();
void RatesPreFrame();
void FrameDustLoaded();
extern bool g_rotateInPlace;                                      // the camera does not turn around the cursor
extern volatile int g_panned;
void __cdecl PanPointer(float* out);
void EditorInit();
void EditorFrame(bool foreground);
void EditorDustLoaded();
void SandboxInit();
void SandboxFrame(bool foreground);
void SandboxDustLoaded();
bool SandboxActive();
enum { PANEL_TEXT, PANEL_HEAD, PANEL_ON, PANEL_DIM };
struct PanelLine { int style; char key[12], text[52]; };          // zeroed before filling
void PanelInit();
void PanelShow(const PanelLine* lines, int n);                    // visible for half a second
void PanelHide();
bool PanelVisible();
void PanelDraw(struct IDirect3DDevice9* d, int width, int height);
void FreeplayInit();
void FreeplayFrame(bool foreground);
void FreeplayDustLoaded();
bool FreeplayActive();
