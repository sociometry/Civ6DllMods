#include "pch.h"
#include "HookAPI.h"
#include "AdjustPlotProperty.h"

extern "C" __declspec(dllexport) void OnLoad(HookContext* ctx) {
    if (!HookContext_IsCompatible(ctx)) {
        return;
    }
    HookAPI* api = HookAPI_Resolve(ctx);
    if (!HookAPI_RequireAll(api, &HookAPI::Log, &HookAPI::ResolveByName)) {
        return;
    }
    api->Log("AdjustPlotPropertyEffect OnLoad HookAPI v%u", api->apiVersion);

    if (AdjustPlotPropertyEffect_Register(api)) {
        api->Log("AdjustPlotPropertyEffect: EFFECT_ADJUST_PLOT_PROPERTY registration OK");
    } else {
        api->Log("AdjustPlotPropertyEffect: EFFECT_ADJUST_PLOT_PROPERTY registration FAILED");
    }
}

extern "C" __declspec(dllexport) void OnUnload() {}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) {
    return TRUE;
}
