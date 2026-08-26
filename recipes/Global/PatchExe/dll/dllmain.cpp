#include "pch.h"
#include "HookAPI.h"

#include <cstdint>

// DX11 CivilizationVI.exe:
// ResourceList.AddResource 容量判断相关字节 @ RVA 0x9407CF。
static constexpr uintptr_t kPatchRva = 0x9407CF;
static constexpr uint8_t kPatchedByte = 0x01;

static HookAPI* g_api = nullptr;
static void* g_target = nullptr;

extern "C" __declspec(dllexport) void OnLoad(HookAPI* api) {
    if (!HookAPI_IsCompatible(api)) return;
    g_api = api;

    g_target = api->ResolveByRva(kHookImageExeDx11, kPatchRva, 1);
    if (!g_target) {
        api->Log("PatchExe: DX11 CivilizationVI.exe RVA 0x%X unavailable",
            static_cast<unsigned>(kPatchRva));
        return;
    }

    const uint8_t original = *static_cast<const uint8_t*>(g_target);
    if (!api->Patch(g_target, &kPatchedByte, sizeof(kPatchedByte))) {
        api->Log("PatchExe: Patch failed @ %p (RVA 0x%X, current=%02X)",
            g_target, static_cast<unsigned>(kPatchRva), original);
        g_target = nullptr;
        return;
    }

    api->Log("PatchExe: patched exe RVA 0x%X @ %p: %02X -> %02X",
        static_cast<unsigned>(kPatchRva), g_target, original, kPatchedByte);
}

extern "C" __declspec(dllexport) void OnUnload() {
    if (g_api && g_target) {
        if (g_api->Unpatch(g_target)) {
            g_api->Log("PatchExe: restored exe RVA 0x%X",
                static_cast<unsigned>(kPatchRva));
        }
        g_target = nullptr;
    }
    g_api = nullptr;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) {
    return TRUE;
}
