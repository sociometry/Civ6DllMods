// dllmain.cpp — 插件入口。
//
// 依据 docs/PLUGIN_GUIDE.md §3 / §6.1：
//   - OnLoad 先做 ABI 防御，再用 RequireAll 声明真正依赖的入口；
//   - 不实现 OnUnload：Loader 负责回收 hook、参数捕获与订阅，
//     模块卸载由引擎或 Loader 完成，插件侧不做清理。
#include "pch.h"
#include "HookAPI.h"
#include "BugFix/UnlockModelLimit/UnlockModelLimit.h"
#include "APIExtension/ModConfig/ModConfig.h"

extern "C" __declspec(dllexport) int OnLoad(HookContext* ctx) {
	// ABI 防御（docs/PLUGIN_GUIDE.md §3.1）
	if (!HookContext_IsCompatible(ctx)) {
		return 0;
	}
	// 按名字解析出本模块私有的接口表。
	HookAPI* api = HookAPI_Resolve(ctx);
	// 只列本插件真正调用的入口：缺任何一个都说明 Loader 版本不匹配，干净退出。
	if (!HookAPI_RequireAll(api,
			&HookAPI::Log,
			&HookAPI::IsImageLoaded,
			&HookAPI::GetImageInfo,        // HookCurrentExeImage() 内部使用
			&HookAPI::ResolveByRva,
			&HookAPI::ResolveByPattern,    // ResolveSite 的 AOB 兜底
			&HookAPI::InstallHook,
			&HookAPI::Patch,
			&HookAPI::Unpatch)) {
		return 0;
	}

	// ModConfig 需要一组 Lua 栈/全局表入口；缺项时只关掉该模块，不影响 exe 域功能。
	// 这份清单必须覆盖 ModConfigCore.h 里用到的**全部** Lua 入口——漏一个就是空指针调用。
	if (HookAPI_RequireAll(api,
			&HookAPI::RegisterGlobalScript,
			&HookAPI::LuaNewGlobalTable,
			&HookAPI::LuaCreateTable,
			&HookAPI::LuaPushCFunctionField,
			&HookAPI::LuaSetField,
			&HookAPI::LuaSetTable,
			&HookAPI::LuaNext,
			&HookAPI::LuaObjLen,
			&HookAPI::LuaRawGetI,
			&HookAPI::LuaGetField,
			&HookAPI::LuaPushValue,
			&HookAPI::LuaPop,
			&HookAPI::LuaPushFString,
			&HookAPI::LuaPushInteger,
			&HookAPI::LuaPushBoolean,
			&HookAPI::LuaPushNil,
			&HookAPI::LuaPushNumber,
			&HookAPI::LuaCheckLString,
			&HookAPI::LuaGetTop,
			&HookAPI::LuaType,
			&HookAPI::LuaToInteger,
			&HookAPI::LuaToNumber,
			&HookAPI::LuaToBoolean)) {
		ModConfig_Init(api);
	} else {
		api->Log("%s", "ModConfig: required lua entries missing - module disabled");
	}

	UnlockModelLimit_Init(api);
	return 1;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) {
	return TRUE;
}
