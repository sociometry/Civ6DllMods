#include "pch.h"
#include "HookAPI.h"
#include "NewRequirements.h"

extern "C" __declspec(dllexport) int OnLoad(HookContext* ctx) {
    if (!HookContext_IsCompatible(ctx)) {
        return 0;
    }
    HookAPI* api = HookAPI_Resolve(ctx);
    if (!HookAPI_RequireAll(api, &HookAPI::Log, &HookAPI::ResolveByName, &HookAPI::InstallHook)) {
        return 0;
    }
    api->Log(
        "NewRequirements OnLoad HookAPI v%u structSize=%u ResolveByRvaEx=%u",
        ctx->apiVersion,
        ctx->structSize,
        static_cast<unsigned>(api->ResolveByRvaEx != nullptr));

    // Comment out either line to disable ONLY that requirement, then rebuild.
    // Each module logs its own result; failure does not skip the other module.
    bool registered = false;
    registered |= NewRequirements_RegisterPlayerProperty(api);
    registered |= NewRequirements_RegisterPlotRoute(api);
    // Keep a working module active even if the other registration failed.
    return registered ? 1 : 0;
}

extern "C" __declspec(dllexport) void OnUnload() {
    NewRequirements_UnloadPlayerProperty();
    NewRequirements_UnloadPlotRoute();
    // Factories and their vtables remain intact for the engine's teardown.
    // The loader frees this DLL only after the engine unloads GameCore.
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) {
    return TRUE;
}
