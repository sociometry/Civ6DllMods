// dllmain.cpp — GameCore 插件入口。
//
// 生命周期（docs/PLUGIN_GUIDE.md §3）：本插件在**进入对局**时加载，此时 GameCore.dll
// 早已加载完毕、pdata 表也建好了——所以这里没有、也不需要任何"等 GameCore 就绪"的
// 代际回调（旧版用的 RegisterGameCoreReady / RegisterGameCoreTeardown 已从 HookAPI.h 删除）。
// 退出对局/回主菜单时整个 GameCore 映像连同本插件一起卸载，Loader 按 owner 自动回滚
// 钩子、脚本登记与消息订阅，因此 OnUnload 保持空实现。
//
// 每个模块的 Init 都自带"解析失败即降级"的保护：单个目标找不到只会关掉对应功能，
// 不影响其它模块，也不会让整个插件加载失败。
//
// 注意：ModConfig 不在这里。它是全局能力，由 Global 侧的
// RegisterGlobalScript 注册 —— 那个入口的语义就是"注册全局可用的接口"，
// loader 会把它挂到每一个脚本环境（主菜单 / 对局 gameplay / 对局 UI）。
#include "pch.h"
#include "HookAPI.h"

#include "BugFix/GreatWorkOptimizer/GreatWorkOptimizer.h"
#include "BugFix/GreatWorkTypeLookup/GreatWorkTypeLookup.h"
#include "BugFix/GreatWorkYieldStack/GreatWorkYieldStack.h"
#include "BugFix/UnitDamageCap/UnitDamageCap.h"
#include "APIExtension/GreatWorkAPI/GreatWorkAPI.h"
#include "APIExtension/ModifierManagerAPI/ModifierManagerAPI.h"
#include "APIExtension/UnitLevelAPI/UnitLevelAPI.h"

extern "C" __declspec(dllexport) int OnLoad(HookContext* ctx) {
	// ABI 防御必须最先做（docs/PLUGIN_GUIDE.md §3.1）
	if (!HookContext_IsCompatible(ctx)) {
		return 0;
	}
	HookAPI* api = HookAPI_Resolve(ctx);
	// 主依赖：所有模块共用的取址/钩子入口 + Lua 表构造入口
	// （GreatWorkAPI / ModifierManagerAPI 会无条件用后者，缺一个就是空指针调用）。
	if (!HookAPI_RequireAll(api,
			&HookAPI::Log,
			&HookAPI::ResolveByName,
			&HookAPI::ResolveByPattern,
			&HookAPI::InstallHook,
			&HookAPI::GetPlayer,
			&HookAPI::RegisterGameCoreScriptForGamePlay,
			&HookAPI::RegisterGameCoreScriptForUI,
			&HookAPI::PushLuaMethods,
			&HookAPI::RegisterPushMethodsTarget,
			&HookAPI::LuaNewGlobalTable,
			&HookAPI::LuaCreateTable,
			&HookAPI::LuaPushCFunctionField,
			&HookAPI::LuaSetField,
			&HookAPI::LuaSetTable,
			&HookAPI::LuaNext,
			&HookAPI::LuaObjLen,
			&HookAPI::LuaRawGetI,
			&HookAPI::LuaPushFString,
			&HookAPI::LuaPushInteger,
			&HookAPI::LuaPushBoolean,
			&HookAPI::LuaPop)) {
		return 0;
	}

	api->Log("%s", "Civ6BasePlugin_GameCore: OnLoad");

	// ---- BugFix ----
	GreatWorkOptimizer_Init(api);
	GreatWorkTypeLookup_Init(api);
	GreatWorkYieldStack_Init(api);
	UnitDamageCap_Init(api);

	// ---- APIExtension ----
	GreatWorkAPI_Init(api);
	ModifierManagerAPI_Init(api);
	UnitLevelAPI_Init(api);

	api->Log("%s", "Civ6BasePlugin_GameCore: loaded");
	return 1;
}

extern "C" __declspec(dllexport) void OnUnload() {
	// 钩子/脚本登记由 Loader 按 owner 统一回收；这里只断引用，避免 detour 跳回已卸载模块。
	GreatWorkOptimizer_Shutdown();
	GreatWorkTypeLookup_Shutdown();
	GreatWorkYieldStack_Shutdown();
	UnitDamageCap_Shutdown();
	GreatWorkAPI_Shutdown();
	ModifierManagerAPI_Shutdown();
	UnitLevelAPI_Shutdown();
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) {
	return TRUE;
}