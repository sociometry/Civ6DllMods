// InitUnitReturn.cpp
// UnitManager.InitUnitValidAdjacentHex 返回 nil 的修复。
//
// ---------------------------------------------------------------------------
// 反编译原文（GameCore_XP2_FinalRelease.dll，IDA）
// ---------------------------------------------------------------------------
//   GameCore::Lua::IUnitManager::lInitUnit（RVA 0x701480）尾部：
//       GameCore::Message::SignalBase::ProtoSignal<...>::Dispatch(...);
//       GameCore::Lua::ScopedInstance<GameCore::Lua::IUnit,GameCore::Unit::Instance>
//           ::Push(lua_State, value: v12);
//       return 1;
//
//   GameCore::Lua::IUnitManager::lInitUnitValidAdjacentHex（RVA 0x7015C0）尾部：
//       172681733  mov rcx, rbx          ; Unit::Instance*
//       172681736  call Unit::Placement::Set
//       17268173b  mov rcx, cs:ms_pkCurrentGame
//       172681745  mov rdx, rbx          ; Unit::Instance*
//       172681756  call ProtoSignal<...>::Dispatch
//       17268175b  mov eax, 1             ; ← 声明返回 1 个值
//       172681760  add rsp, 80h
//       17268176b  retn
//   —— 全程没有任何 Push，Lua 侧因此拿到 nil。
//
// ---------------------------------------------------------------------------
// 做法（两段钩子）
// ---------------------------------------------------------------------------
//   Hook A：lInitUnitValidAdjacentHex（RVA 0x7015C0，序言唯一）
//     入口：记下 Lua 状态 L、记下当前栈顶，置 inCall = true
//     出口：inCall = false
//           若 inCall 期间没有发生任何 Push（栈顶没变），且原函数返回 1，
//           说明本次真的没压值 —— 用 Hook B 截到的 unit 补一次 Push。
//
//   Hook B：GameCore::Unit::Placement::Set（RVA 0x3B11B0，序言唯一）
//     static 函数，签名 (Unit::Instance*, PlotCoord const&, uint, uint)。
//     rcx 就是那个单位实例。inCall 为真时把它存进 pendingUnit。
//
// 为什么要在"A 的出口"补而不是"B 里立刻 Push"：
//   原函数在 B 之后还要 Dispatch(UnitPlaced) 并做收尾。若在 B 里就 Push，
//   之后原函数自己再 Push 一次（将来版本修好）就会返回两个值，Lua 侧多一个
//   返回值同样会出错。放在 A 的出口补，既能确认"原函数确实没压"，又保证
//   顺序在 Dispatch 之后 —— 与 lInitUnit 的既有行为一致。
//
// ---------------------------------------------------------------------------
// 红线
// ---------------------------------------------------------------------------
//   1) 绝不在 Placement::Set 内部直接 Push。Set 是全局工具，还有别的调用者。
//   2) inCall 用 thread_local：万一有嵌套/重入，也不会串味到别的调用上。
//   3) 补 Push 前一定比对栈顶：原函数哪天自己修好了，我们自动退化为"什么都不做"。
//
// 线程模型：OnLoad 与两个 detour 都在游戏主线程。

#include "pch.h"
#include "HookAPI.h"
#include "InitUnitReturn.h"
#include "../../Common/GameCoreResolve.h"

#include <cstdint>

namespace {

// ---------------------------------------------------------------------------
// 目标
// ---------------------------------------------------------------------------

// lInitUnitValidAdjacentHex（RVA 0x7015C0）
//   int __fastcall (lua_State* L)   —— 只取 1 个参数，返回压栈的返回值个数
constexpr HookInfo kTargetLuaInitUnitAdj = HookName(
	"GameCore::Lua::IUnitManager::lInitUnitValidAdjacentHex",
	"40 53 55 41 56 48 81 EC 80 00 00 00 BA 01 00 00 00 48 8B D9");

// Unit::Placement::Set（RVA 0x3B11B0）
//   PLACEMENT_RESULT __fastcall (Unit::Instance*, PlotCoord const&, uint, uint)
constexpr HookInfo kTargetPlacementSet = HookName(
	"GameCore::Unit::Placement::Set",
	"40 55 57 41 54 41 55 48 8D AC 24 48 FF FF FF 48 81 EC B8 01 00 00");

// ScopedInstance<IUnit, Unit::Instance>::Push（RVA 0x1EFF0）
//   static void __fastcall (lua_State*, Unit::Instance*)
// 该序言在库里多处命中（模板实例），因此只走符号名。
constexpr HookInfo kTargetUnitPush = HookName(
	"?Push@?$ScopedInstance@VIUnit@Lua@GameCore@@VInstance@Unit@3@@Lua@GameCore@@"
	"SAXPEAUlua_State@@PEAVInstance@Unit@3@@Z",
	nullptr);

// ---------------------------------------------------------------------------
// 函数指针
// ---------------------------------------------------------------------------
typedef int (__fastcall* LuaFn)(void* L);
typedef int (__fastcall* PlacementSetFn)(
	void* unit, const void* plotCoord, unsigned int a3, unsigned int a4);
typedef void (__fastcall* UnitPushFn)(void* L, void* unit);

HookAPI* g_api = nullptr;
LuaFn           g_origLuaInitUnitAdj = nullptr;
PlacementSetFn  g_origPlacementSet = nullptr;
UnitPushFn      g_unitPush = nullptr;

// ---------------------------------------------------------------------------
// 调用上下文
// ---------------------------------------------------------------------------
struct CallContext {
	bool  active = false;   // 当前是否正处于 lInitUnitValidAdjacentHex 内
	void* unit   = nullptr; // Placement::Set 截到的单位实例
	int   topAtEntry = 0;   // 进入时的 Lua 栈顶
};

thread_local CallContext g_ctx;

// ---------------------------------------------------------------------------
// Detour B：截单位实例
// ---------------------------------------------------------------------------
int __fastcall Hook_PlacementSet(
	void* unit, const void* plotCoord, unsigned int a3, unsigned int a4) {
	if (!g_origPlacementSet) return 0;

	// 只在 lInitUnitValidAdjacentHex 的调用链里记；其它调用者一概不碰。
	if (g_ctx.active && unit && !g_ctx.unit) {
		g_ctx.unit = unit;
	}
	return g_origPlacementSet(unit, plotCoord, a3, a4);
}

// ---------------------------------------------------------------------------
// Detour A：包住 lInitUnitValidAdjacentHex，出口补 Push
// ---------------------------------------------------------------------------
int __fastcall Hook_LuaInitUnitValidAdjacentHex(void* L) {
	if (!g_origLuaInitUnitAdj) return 0;

	// 保存外层状态（正常情况下为空，重入时也不会串）
	const CallContext saved = g_ctx;

	g_ctx.active = true;
	g_ctx.unit = nullptr;
	g_ctx.topAtEntry = (g_api && L) ? g_api->LuaGetTop(L) : 0;

	const int topBefore = g_ctx.topAtEntry;
	const int pushed = g_origLuaInitUnitAdj(L);

	void* captured = g_ctx.unit;
	g_ctx = saved;   // 先还原，后续逻辑不再受本次调用影响

	if (pushed <= 0 || !captured || !g_unitPush || !L) {
		return pushed;
	}

	// 原函数「声称」返回 pushed 个值。若实际压栈数已经够，说明它自己压过
	// （将来版本修好、或别处补过），我们就什么都不做 —— 绝不返回两个值。
	const int topAfter = g_api ? g_api->LuaGetTop(L) : 0;
	if (topAfter - topBefore >= pushed) {
		return pushed;
	}

	g_unitPush(L, captured);
	if (g_api) {
		g_api->Log("%s",
			"[InitUnitReturn] InitUnitValidAdjacentHex returned nil - pushed unit instance");
	}
	return pushed;
}

} // namespace

void InitUnitReturn_Init(HookAPI* api) {
	if (!api) return;
	g_api = api;
	g_origLuaInitUnitAdj = nullptr;
	g_origPlacementSet = nullptr;
	g_unitPush = nullptr;

	g_unitPush = gc::ResolveVerified<UnitPushFn>(api, kHookImageGameCoreXP2,
		kTargetUnitPush, nullptr, 0, "InitUnitReturn/ScopedInstancePush");
	if (!g_unitPush) {
		api->Log("%s", "[InitUnitReturn] ScopedInstance<IUnit,Unit>::Push unresolved - skipped");
		return;
	}

	// 先装 B：A 里一旦调用就会用到它
	void* setTarget = gc::ResolveVerified<void*>(api, kHookImageGameCoreXP2,
		kTargetPlacementSet, nullptr, 0, "InitUnitReturn/Placement::Set");
	if (!setTarget) {
		api->Log("%s", "[InitUnitReturn] Unit::Placement::Set unresolved - skipped");
		return;
	}
	if (!api->InstallHook(setTarget, reinterpret_cast<void*>(&Hook_PlacementSet),
			reinterpret_cast<void**>(&g_origPlacementSet))) {
		api->Log("%s", "[InitUnitReturn] InstallHook(Placement::Set) failed");
		return;
	}

	void* luaTarget = gc::ResolveVerified<void*>(api, kHookImageGameCoreXP2,
		kTargetLuaInitUnitAdj, nullptr, 0, "InitUnitReturn/lInitUnitValidAdjacentHex");
	if (!luaTarget) {
		api->Log("%s", "[InitUnitReturn] lInitUnitValidAdjacentHex unresolved - skipped");
		return;
	}
	if (!api->InstallHook(luaTarget, reinterpret_cast<void*>(&Hook_LuaInitUnitValidAdjacentHex),
			reinterpret_cast<void**>(&g_origLuaInitUnitAdj))) {
		api->Log("%s", "[InitUnitReturn] InstallHook(lInitUnitValidAdjacentHex) failed");
		return;
	}

	api->Log("%s",
		"[InitUnitReturn] active: InitUnitValidAdjacentHex now returns the unit instance");
}

void InitUnitReturn_Shutdown() {
	g_ctx = CallContext{};
	g_origLuaInitUnitAdj = nullptr;
	g_origPlacementSet = nullptr;
	g_unitPush = nullptr;
	g_api = nullptr;
}