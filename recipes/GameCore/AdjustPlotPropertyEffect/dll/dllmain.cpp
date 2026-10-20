#include "pch.h"
#include "HookAPI.h"
#include "AdjustPlotProperty.h"

extern "C" __declspec(dllexport) int OnLoad(HookContext* ctx) {
    if (!HookContext_IsCompatible(ctx)) {
        return 0;
    }
    HookAPI* api = HookAPI_Resolve(ctx);
    if (!HookAPI_RequireAll(api, &HookAPI::Log, &HookAPI::ResolveByName)) {
        return 0;
    }
    api->Log("AdjustPlotPropertyEffect OnLoad HookAPI v%u", api->apiVersion);

    const bool registered = AdjustPlotPropertyEffect_Register(api);
    if (registered) {
        api->Log("AdjustPlotPropertyEffect: EFFECT_ADJUST_PLOT_PROPERTY registration OK");
    } else {
        api->Log("AdjustPlotPropertyEffect: EFFECT_ADJUST_PLOT_PROPERTY registration FAILED");
    }
    return registered ? 1 : 0;
}

extern "C" __declspec(dllexport) void OnUnload() {}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) {
    return TRUE;
}
