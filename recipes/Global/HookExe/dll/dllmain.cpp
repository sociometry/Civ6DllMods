#include "pch.h"
#include "HookAPI.h"

#include <atomic>
#include <cstdint>

// Global DX11 example: preserve the native AddResource result and count calls.
// The EXE hook and its counter live across GameCore unload/reload cycles.
static constexpr uintptr_t kRva_AddResource = 0x940770;
static HookAPI* g_api = nullptr;
using AddResourceFn = __int64(__fastcall*)(int32_t* list);
static AddResourceFn g_origAddResource = nullptr;
static std::atomic<uint64_t> g_callCount{0};
static bool g_installed = false;

static bool LooksLikeCodePrologue(const void* addr) {
    const auto* p = static_cast<const unsigned char*>(addr);
    return p[0] != 0x00 && p[0] != 0xCC && p[0] != 0x90;
}

static __int64 __fastcall Hook_AddResource(int32_t* list) {
    const uint64_t n = g_callCount.fetch_add(1, std::memory_order_relaxed) + 1;
    const __int64 id = g_origAddResource(list);
    if (n == 1 || n % 200 == 0) {
        g_api->Log("HookExe: AddResource call #%llu list=%p -> id=%lld",
            static_cast<unsigned long long>(n), list, static_cast<long long>(id));
    }
    return id;
}

extern "C" __declspec(dllexport) void OnLoad(HookContext* ctx) {
    if (!HookContext_IsCompatible(ctx)) return;
    HookAPI* api = HookAPI_Resolve(ctx);
    if (!api->Log || !api->IsImageLoaded || !api->ResolveByRva || !api->InstallHook) return;
    if (g_installed) {
        api->Log("HookExe: already installed; keeping current hook and call count");
        return;
    }
    if (!api->IsImageLoaded(kHookImageExeDx11)) {
        api->Log("HookExe: DX11 executable not loaded; skipping (DX12 unsupported)");
        return;
    }
    void* target = api->ResolveByRva(kHookImageExeDx11, kRva_AddResource, 1);
    if (!target || !LooksLikeCodePrologue(target)) {
        api->Log("HookExe: AddResource target unavailable or prologue check failed");
        return;
    }
    // The loader assigns original before publishing our detour. Initialize the
    // state it uses before InstallHook, as the first call can happen immediately.
    g_api = api;
    if (!api->InstallHook(target, reinterpret_cast<void*>(&Hook_AddResource),
            reinterpret_cast<void**>(&g_origAddResource))) {
        api->Log("HookExe: InstallHook failed; no hook installed by this attempt");
        return;
    }
    g_installed = true;
    api->Log("HookExe: hooked exe AddResource @ %p (anchor RVA 0x%X)",
        target, static_cast<unsigned>(kRva_AddResource));
    // Global plugins must not register gameplay/UI Lua globals. Use the log
    // above to observe the counter without retaining any GameCore Lua state.
}

extern "C" __declspec(dllexport) void OnUnload() {
    if (g_api) {
        g_api->Log("HookExe: OnUnload; total calls=%llu (loader owns hook cleanup)",
            static_cast<unsigned long long>(g_callCount.load(std::memory_order_relaxed)));
    }
    // Do not clear original or shared state here: the loader detaches the hook
    // after OnUnload, and an already entered detour can still need its trampoline.
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
