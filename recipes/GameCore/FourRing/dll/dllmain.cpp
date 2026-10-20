#include "pch.h"
#include "HookAPI.h"

// Hex ring plot count: R0=1, R1=7, R2=19, R3=37, R4=61.
// Every caller computes "ComputePlotsFromRadius(2) + 18", so returning 61 here would
// yield 61 + 18 = 79 and overrun the 61-entry ring offset tables. Returning 43 makes
// all call sites land on 43 + 18 = 61, i.e. exactly the 4-ring plot count.
static constexpr int kPlotsForRing4 = 43;

static HookAPI* g_api = nullptr;

using ComputePlotsFromRadiusFn = __int64(__fastcall*)(int radius);
static ComputePlotsFromRadiusFn g_orig = nullptr;

static __int64 __fastcall Hook_ComputePlotsFromRadius(int /*radius*/) {
    return kPlotsForRing4;
}

// 必须返回 int：Loader 用 `onLoad(ctx) != 0` 判成功（PluginRegistry.cpp 的 CallOnLoadSafe）。
// 写成 void 会让那个判断读到上一条调用的遗留 eax，插件会被随机判为初始化失败并隔离。
extern "C" __declspec(dllexport) int OnLoad(HookContext* ctx) {
    if (!HookContext_IsCompatible(ctx)) {
        return 0;
    }
    // 按名字解析出本模块私有的接口表；查不到的入口为 nullptr。
    HookAPI* api = HookAPI_Resolve(ctx);
    if (!HookAPI_RequireAll(api, &HookAPI::ResolveByName, &HookAPI::InstallHook, &HookAPI::Log)) {
        return 0;
    }
    g_api = api;

    void* target = api->ResolveByName(
        kHookImageGameCoreXP2,
        "GameCore::City::CityHexspace::ComputePlotsFromRadius");
    if (!target) {
        api->Log("FourRing: ComputePlotsFromRadius not found");
        return 0;
    }

    if (!api->InstallHook(target, reinterpret_cast<void*>(&Hook_ComputePlotsFromRadius),
            reinterpret_cast<void**>(&g_orig))) {
        api->Log("FourRing: InstallHook failed");
        return 0;
    }

    api->Log("FourRing: hooked ComputePlotsFromRadius -> always %d (RVA target=%p)",
        kPlotsForRing4, target);
    return 1;
}

extern "C" __declspec(dllexport) void OnUnload() {}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) {
    return TRUE;
}
