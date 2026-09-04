#include "pch.h"
#include "HookAPI.h"
#include "AdjustPlotProperty.h"

extern "C" __declspec(dllexport) void OnLoad(HookAPI* api) {
    if (!HookAPI_IsCompatible(api)) {
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
