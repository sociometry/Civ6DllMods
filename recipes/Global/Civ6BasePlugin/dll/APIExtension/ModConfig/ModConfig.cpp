// ModConfig.cpp —— Global 侧入口：把 ModConfig 注入引擎主 Lua state。
//
// 为什么挂在 Global 插件里：
//   * RegisterGlobalScript 接的是 exe 的进程级 Lua VM（换局不失效），主菜单 UI 与
//     对局内脚本都能看到同一个 ModConfig 表；
//   * GameCore 插件（随对局装卸）另有同名模块，负责把接口注入对局的 gameplay / UI
//     两个脚本环境——两边读写同一批 JSON 文件，靠 ModConfigCore 的 mtime 缓存保持一致。

#include "pch.h"
#include "HookAPI.h"
#include "ModConfig.h"
#include "ModConfigCore.h"

namespace {

HookAPI* g_api = nullptr;

// RegisterGlobalScript 回调：在引擎主 state 上挂 ModConfig。
void RegisterMainLuaGlobals(void* L) {
	if (!g_api) return;
	civ6base::modconfig::SetApi(g_api);
	civ6base::modconfig::RegisterLuaTable(L);
}

} // namespace

void ModConfig_Init(HookAPI* api) {
	if (!api) return;
	g_api = api;
	civ6base::modconfig::SetApi(api);

	if (!api->RegisterGlobalScript) {
		api->Log("%s", "[ModConfig] RegisterGlobalScript unavailable - module disabled");
		return;
	}
	api->RegisterGlobalScript(&RegisterMainLuaGlobals);
	api->Log("%s", "[ModConfig] module initialized (main lua state)");
}

void ModConfig_Shutdown() {
	// 全局脚本登记由 Loader 按 owner 自动摘除；这里只断引用。
	g_api = nullptr;
	civ6base::modconfig::SetApi(nullptr);
}
