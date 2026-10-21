// UnitLevelAPI.cpp
// 给 Unit::Experience 补 SetLevel（引擎只有读，没有对外的写）。
//
// ---------------------------------------------------------------------------
// 目标函数（已用 tools/rva_to_symbol.exe 核对 RVA）
// ---------------------------------------------------------------------------
//   Experience::SetLevel   RVA 0x3974A0
//     反汇编：
//       13f3b74a0  sub rsp, 38h
//       13f3b74a4  mov rax, [rcx]              ; m_pUnit
//       13f3b74aa  mov [rcx+10h], edx          ; m_iLevel = level
//       13f3b74b2  mov [rsp+..], 0A038D885h    ; UNIT_EXPERIENCE_CHANGED
//       13f3b74e7  call Event::Send
//       13f3b74f0  retn
//     —— 只写等级 + 发事件，不动经验值 +0x0C（引擎本来如此）。
//
//   Lua Experience::GetInstance   RVA 0x6FFED0
//     GameCore::Lua::ScopedInstance<GameCore::Lua::IUnitExperience,GameCore::Unit::Experience>::GetInstance
//     从 Lua 栈第 1 参数取 Unit::Experience*。
//
// ---------------------------------------------------------------------------
// 挂载点
// ---------------------------------------------------------------------------
// 目标表 "GameCore::Lua::IUnitExperience"（Lua 侧对象名 "Unit::Experience"），
// 与 GetLevel / GetExperiencePoints / ChangeExperience 等同族接口同表。
// 调用形式：unit:GetExperience():SetLevel(n)
//
// 线程模型：OnLoad 与全部 Lua 回调都在游戏主线程。

#include "pch.h"
#include "HookAPI.h"
#include "UnitLevelAPI.h"
#include "../../Common/GameCoreResolve.h"

#include <cstdint>

namespace {

// ---- Experience::SetLevel：符号名唯一 ----
constexpr HookInfo kTargetSetLevel = HookName(
	"GameCore::Unit::Experience::SetLevel",
	"48 83 EC 38 48 8B 01 44 8B C2 89 51 10 48 8D 54");
constexpr uint8_t kSigSetLevel[8] = { 0x48, 0x83, 0xEC, 0x38, 0x48, 0x8B, 0x01, 0x44 };

constexpr int kLuaTNumber = 3;

typedef void(__fastcall* SetLevelFn)(void* experience, int level);
typedef void*(__fastcall* GetExperienceInstanceFn)(void* L, int index, bool required);

HookAPI* g_api = nullptr;
SetLevelFn g_setLevel = nullptr;
GetExperienceInstanceFn g_getExperience = nullptr;

// unit:GetExperience():SetLevel(level) -> boolean
int lSetLevel(void* L) {
	if (!g_api) return 0;
	void* experience = g_getExperience ? g_getExperience(L, 1, true) : nullptr;
	if (!experience || !g_setLevel) {
		g_api->LuaPushBoolean(L, false);
		return 1;
	}
	if (g_api->LuaGetTop(L) < 2 || g_api->LuaType(L, 2) != kLuaTNumber) {
		g_api->LuaPushBoolean(L, false);
		return 1;
	}
	int level = static_cast<int>(g_api->LuaToNumber(L, 2));
	if (level < 0) level = 0;
	g_setLevel(experience, level);
	g_api->LuaPushBoolean(L, true);
	return 1;
}

void InjectExperienceMethod() {
	if (!g_api) return;
	// 与引擎同族接口同表：unit:GetExperience():Xxx()
	static LuaMethodInfo s_methods[] = {
		{ &lSetLevel, "lSetLevel", "SetLevel" },
	};
	const HookApiPushMethodsResult pushed = g_api->PushLuaMethods(
		"GameCore::Lua::IUnitExperience::PushMethods", kHookApiPushNoManager, s_methods,
		static_cast<uint32_t>(sizeof(s_methods) / sizeof(s_methods[0])));
	g_api->Log("[UnitLevelAPI] experience-method inject: pushed=%d "
		"(unit:GetExperience():SetLevel(n))",
		static_cast<int>(pushed));
}

} // namespace

void UnitLevelAPI_Init(HookAPI* api) {
	if (!api) return;
	g_api = api;

	g_setLevel = gc::ResolveVerified<SetLevelFn>(api, kHookImageGameCoreXP2,
		kTargetSetLevel, kSigSetLevel, sizeof(kSigSetLevel), "UnitLevelAPI/SetLevel");

	constexpr HookInfo kTargetExpGetInstance = HookName(
		"GameCore::Lua::ScopedInstance<GameCore::Lua::IUnitExperience,GameCore::Unit::Experience>::GetInstance");
	constexpr uint8_t kSigScopedInstance[8] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C };
	g_getExperience = gc::ResolveVerified<GetExperienceInstanceFn>(api, kHookImageGameCoreXP2,
		kTargetExpGetInstance, kSigScopedInstance, sizeof(kSigScopedInstance),
		"UnitLevelAPI/Experience::GetInstance");

	if (!g_setLevel || !g_getExperience) {
		api->Log("[UnitLevelAPI] incomplete: setLevel=%d getExperience=%d",
			g_setLevel != nullptr, g_getExperience != nullptr);
		return;
	}

	InjectExperienceMethod();
}

void UnitLevelAPI_Shutdown() {
	g_setLevel = nullptr;
	g_getExperience = nullptr;
	g_api = nullptr;
}