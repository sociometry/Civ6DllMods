#include "pch.h"
#include "HookAPI.h"
#include "NewRequirements.h"

extern "C" __declspec(dllexport) void OnLoad(HookAPI* api) {
    if (!HookAPI_IsCompatible(api)) {
        return;
    }
    api->Log(
        "NewRequirements OnLoad HookAPI v%u structSize=%u ResolveByRvaEx=%u",
        api->apiVersion,
        api->structSize,
        static_cast<unsigned>(api->structSize >= sizeof(HookAPI) + sizeof(void*)));

    // Comment out either line to disable ONLY that requirement, then rebuild.
    // Each module logs its own result; failure does not skip the other module.
    NewRequirements_RegisterPlayerProperty(api);
    NewRequirements_RegisterPlotRoute(api);
}

extern "C" __declspec(dllexport) void OnUnload() {}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) {
    return TRUE;
}
