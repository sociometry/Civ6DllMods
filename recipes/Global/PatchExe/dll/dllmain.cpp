#include "pch.h"
#include "HookAPI.h"
#include "PatchTargetSignature.h"

#include <cstdint>
#include <cstring>

// Signature coordinates refer to the DX11 anchor executable. ResolveByRva
// translates them through the loader's pdata table before byte validation.
// Original example behavior: cmp eax, 0x2000 becomes cmp eax, 0x100.
static constexpr uint8_t kPatchedByte = 0x01;
static HookAPI* g_api = nullptr;
static void* g_target = nullptr;

extern "C" __declspec(dllexport) void OnLoad(HookAPI* api) {
    if (!HookAPI_IsCompatible(api)) return;
    if (!api->Log || !api->IsImageLoaded || !api->ResolveByRva || !api->Patch || !api->Unpatch) return;
    if (g_target) {
        api->Log("PatchExe: already patched; keeping the original rollback record");
        return;
    }
    if (!api->IsImageLoaded(kHookImageExeDx11)) {
        api->Log("PatchExe: DX11 executable not loaded; skipping (DX12 unsupported)");
        return;
    }
    auto* window = static_cast<uint8_t*>(api->ResolveByRva(
        kHookImageExeDx11, kPatchWindowRva, sizeof(kPatchExpectedBytes)));
    if (!window) {
        api->Log("PatchExe: patch instruction window unavailable");
        return;
    }
    if (std::memcmp(window, kPatchExpectedBytes, sizeof(kPatchExpectedBytes)) != 0) {
        api->Log("PatchExe: instruction signature mismatch; patch skipped");
        return;
    }
    static_assert(kPatchByteOffset < sizeof(kPatchExpectedBytes));
    void* target = window + kPatchByteOffset;
    const uint8_t original = kPatchExpectedBytes[kPatchByteOffset];
    if (!api->Patch(target, &kPatchedByte, sizeof(kPatchedByte))) {
        api->Log("PatchExe: Patch failed @ %p; no patch installed by this attempt", target);
        return;
    }
    g_api = api;
    g_target = target;
    api->Log("PatchExe: patched exe anchor RVA 0x%X @ %p: %02X -> %02X",
        static_cast<unsigned>(kPatchWindowRva + kPatchByteOffset), target,
        static_cast<unsigned>(original), static_cast<unsigned>(kPatchedByte));
}

extern "C" __declspec(dllexport) void OnUnload() {
    if (!g_target) return;
    if (!g_api->Unpatch(g_target)) {
        // Restore failed (e.g. another plugin stacked a patch on this address after
        // ours, so ours is no longer on top of the stack). The loader does not
        // roll back patches for us: the byte stays patched until process exit.
        // Keep the record so a later call can retry.
        g_api->Log("PatchExe: restore failed @ %p; patch remains applied", g_target);
        return;
    }
    g_api->Log("PatchExe: restored exe anchor RVA 0x%X",
        static_cast<unsigned>(kPatchWindowRva + kPatchByteOffset));
    g_target = nullptr;
    g_api = nullptr;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
