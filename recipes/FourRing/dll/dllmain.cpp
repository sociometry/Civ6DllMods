#include "pch.h"
#include "HookAPI.h"

// Hex ring plot count: R0=1, R1=7, R2=19, R3=37, R4=61.
// Forcing 61 unlocks 4-ring workable / district placement checks that use this helper.
static constexpr int kPlotsForRing4 = 61;

static HookAPI* g_api = nullptr;

using ComputePlotsFromRadiusFn = __int64(__fastcall*)(int radius);
static ComputePlotsFromRadiusFn g_orig = nullptr;

static __int64 __fastcall Hook_ComputePlotsFromRadius(int /*radius*/) {
    return kPlotsForRing4;
}

extern "C" __declspec(dllexport) void OnLoad(HookAPI* api) {
    if (!HookAPI_IsCompatible(api)) {
        return;
    }
    g_api = api;

    void* target = api->RequireFunction(
        "GameCore::City::CityHexspace::ComputePlotsFromRadius");
    if (!target) {
        api->Log("FourRing: ComputePlotsFromRadius not found");
        return;
    }

    if (!api->InstallHook(target, reinterpret_cast<void*>(&Hook_ComputePlotsFromRadius),
            reinterpret_cast<void**>(&g_orig))) {
        api->Log("FourRing: InstallHook failed");
        return;
    }

    api->Log("FourRing: hooked ComputePlotsFromRadius -> always %d (RVA target=%p)",
        kPlotsForRing4, target);
}

extern "C" __declspec(dllexport) void OnUnload() {}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) {
    return TRUE;
}
