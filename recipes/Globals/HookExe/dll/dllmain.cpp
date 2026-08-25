#include "pch.h"
#include "HookAPI.h"

#include <atomic>
#include <cstdint>

// =============================================================================
// HookExe —— Global 插件 hook 游戏 exe（CivilizationVI.exe）示范
//
// 目标：sub_140940770 @ RVA 0x940770（ResourceList.AddResource 一类分配）
// 反编译见同目录 sub_140940770.txt
// =============================================================================

static HookAPI* g_api = nullptr;

static constexpr uintptr_t kRva_AddResource = 0x940770;

using AddResourceFn = __int64(__fastcall*)(int32_t* list);
static AddResourceFn g_origAddResource = nullptr;
static std::atomic<uint64_t> g_callCount{ 0 };

static DWORD ExeSizeOfImage(HMODULE exe) {
    if (!exe) return 0;
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const uint8_t*>(exe) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return nt->OptionalHeader.SizeOfImage;
}

static void* ResolveExeRva(uintptr_t rva) {
    HMODULE exe = GetModuleHandleW(nullptr);
    const DWORD imageSize = ExeSizeOfImage(exe);
    if (!exe || imageSize == 0 || rva == 0 || rva >= imageSize) {
        return nullptr;
    }
    auto* p = reinterpret_cast<uint8_t*>(exe) + rva;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return nullptr;
    if (mbi.State != MEM_COMMIT) return nullptr;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return nullptr;
    return p;
}

static bool LooksLikeCodePrologue(const void* addr) {
    const auto* p = static_cast<const unsigned char*>(addr);
    if (p[0] == 0x00 || p[0] == 0xCC || p[0] == 0x90) return false;
    return true;
}

static __int64 __fastcall Hook_AddResource(int32_t* list) {
    const uint64_t n = g_callCount.fetch_add(1, std::memory_order_relaxed) + 1;
    const __int64 id = g_origAddResource ? g_origAddResource(list) : 0;
    // 资源分配可能很频繁：首调 + 每 200 次打一次，便于确认 hook 生效。
    if (g_api && (n == 1 || (n % 200) == 0)) {
        g_api->Log("HookExe: AddResource call #%llu list=%p -> id=%lld",
            static_cast<unsigned long long>(n), list,
            static_cast<long long>(id));
    }
    return id;
}

static int __cdecl lGetCallCount(void* L) {
    g_api->LuaPushInteger(L, static_cast<int>(
        g_callCount.load(std::memory_order_relaxed)));
    return 1;
}

static void RegisterGlobals(void* L) {
    g_api->LuaNewGlobalTable(L, "HookExe", 0, 1);
    g_api->LuaPushCFunctionField(L, -1, "GetCallCount",
        reinterpret_cast<void*>(&lGetCallCount), "lHookExe_GetCallCount");
    g_api->LuaPop(L, 1);
}

extern "C" __declspec(dllexport) void OnLoad(HookAPI* api) {
    if (!HookAPI_IsCompatible(api)) {
        return;
    }
    g_api = api;

    void* target = ResolveExeRva(kRva_AddResource);
    if (!target) {
        api->Log("HookExe: AddResource RVA 0x%X out of range / unreadable",
            static_cast<unsigned>(kRva_AddResource));
        return;
    }
    if (!LooksLikeCodePrologue(target)) {
        api->Log("HookExe: prologue check failed at %p (exe updated?)", target);
        return;
    }

    if (!api->InstallHook(target, reinterpret_cast<void*>(&Hook_AddResource),
            reinterpret_cast<void**>(&g_origAddResource))) {
        api->Log("HookExe: InstallHook failed (already hooked?)");
        return;
    }

    api->Log("HookExe: hooked exe AddResource @ %p (RVA 0x%X)",
        target, static_cast<unsigned>(kRva_AddResource));
    api->RegisterScriptGlobals(&RegisterGlobals);
}

extern "C" __declspec(dllexport) void OnUnload() {}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) {
    return TRUE;
}
