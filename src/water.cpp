// Improved water: replaces the game's water shaders (see shaders/water.hlsl) and draws the scene
// behind the water at full resolution.
#include "common.h"
#include "water_shaders.h"

static DWORD Crc(const void* data, size_t n) {
    auto p = static_cast<const uint8_t*>(data);
    DWORD c = ~0u;
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; i++) c = c >> 1 ^ (c & 1 ? 0xEDB88320 : 0);
    }
    return ~c;
}
// Size of a shader in bytes.
static size_t Length(const DWORD* code) {
    const DWORD* p = code + 1;
    while (*p != 0xffff) {
        p += 1 + ((*p & 0xffff) == 0xfffe ? *p >> 16 & 0x7fff : *p >> 24 & 0xf);
        if (p - code > 16384) return 0;
    }
    return (p + 1 - code) * 4;
}
static const DWORD* Swap(const DWORD* code) {
    static const struct { size_t size; DWORD crc; const BYTE* to; const char* name; } list[] = {
        { 2980, 0x53f7b68d, g_waterDeferred, "deferred" },
        { 2784, 0x4ade5a84, g_waterForward, "forward" },
        { 804, 0xfd66be4e, g_waterForwardVS, "forward vertex" },
    };
    if (!code) return code;
    size_t n = Length(code);
    for (auto& s : list) {
        if (n != s.size || Crc(code, n) != s.crc) continue;
        Log("[water] %s shader replaced", s.name);
        return reinterpret_cast<const DWORD*>(s.to);
    }
    return code;
}

// Shader creation: 53faf0 pixel, 53fa90 vertex. The forward pixel and vertex shader are a pair.
typedef void*(__fastcall* CreateT)(uintptr_t, void*, int*, const DWORD*);
static CreateT oPixel, oVertex;
static void* __fastcall hPixel(uintptr_t self, void* edx, int* size, const DWORD* code) { return oPixel(self, edx, size, Swap(code)); }
static void* __fastcall hVertex(uintptr_t self, void* edx, int* size, const DWORD* code) { return oVertex(self, edx, size, Swap(code)); }

// Replace movzx eax, word ptr [esp+28] and [esp+38], with the value doubled.
static __declspec(naked) void WidthStub() {
    __asm {
        movzx eax, word ptr [esp + 0x2c]
        add eax, eax
        ret
    }
}
static __declspec(naked) void HeightStub() {
    __asm {
        movzx eax, word ptr [esp + 0x3c]
        add eax, eax
        ret
    }
}
static void Call(uintptr_t va, void* to) {
    uint8_t b[5] = { 0xE8 };
    *(int32_t*)(b + 1) = (int32_t)((uintptr_t)to - (va + 5));
    WriteMem(va, b, 5);
}

void WaterInit() {
    if (!IniInt("Graphics", "ImprovedWater", 1)) return;
    const char* entry = "51 8b 81 10 27 00 00 8d 14 24 52 8b 54 24 10";
    if (!BytesMatch(0x53faf0, entry) || !BytesMatch(0x53fa90, entry)) { Log("[water] shader code differs, skipped"); return; }
    oVertex = (CreateT)Hook("vertex shaders", 0x53fa90, entry, 7, (void*)hVertex);
    if (!oVertex) return;
    oPixel = (CreateT)Hook("pixel shaders", 0x53faf0, entry, 7, (void*)hPixel);
    // reducedColorBuffer is half the screen size. The water blob's buffers share the values and
    // keep theirs.
    const char *plain = "d1 e9 6a 01 d1 e8 89 4c 24 40 6a 00 0f b7 c9 89 44 24 3c 51 8b 4e 0c 0f b7 c0 50";
    const char *aa1 = "0f b7 44 24 28 8b 4e 0c 8b 11 8b 52 0c 6a 00 68 00 00 01 00", *aa2 = "0f b7 44 24 38 50 6a 00 68 6c f1 89 00";
    if (!BytesMatch(0x43a8d2, plain) || !BytesMatch(0x43a407, aa1) || !BytesMatch(0x43a420, aa2)) { Log("[water] refraction code differs, stays at half resolution"); return; }
    WriteMem(0x43a8de, "\x01\xc9\x90", 3);       // movzx ecx, cx -> add ecx, ecx
    WriteMem(0x43a8e9, "\x01\xc0\x90", 3);       // movzx eax, ax -> add eax, eax
    Call(0x43a407, (void*)WidthStub);
    Call(0x43a420, (void*)HeightStub);
    Log("[water] refraction at full resolution");
}
