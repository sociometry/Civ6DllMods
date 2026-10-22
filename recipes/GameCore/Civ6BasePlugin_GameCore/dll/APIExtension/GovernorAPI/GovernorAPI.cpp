// GovernorAPI.cpp
// Player:GetGovernors():SetAvailableGovPoints(n) 的实现。
//
// 引擎侧事实（IDA 反编译 / 反汇编核实，GameCore_XP2_FinalRelease.dll）：
//
//   GameCore::Player::Governors 布局（本构建）：
//     +0x70 (112)  Player::Instance*   pPlayer
//     +0x118(280)  int                 governorPointsTotal   ← 总量，不是可用
//   可用点数没有自己的字段，是 total - GetGovernorPointsSpent() 的差值。
//
//   ChangeGovernorPoints(this, int delta)  @RVA 0x225E260
//     mov rcx, [rcx+70h] ; this = pPlayer
//     call Player::Instance::IsMajor       ; 非主要文明直接跳过改动
//     if (delta > 0) { total += delta; clamp; HandleGovernorTitleMessaging(); }
//     Event::Send(0xDA3EF23D /*GOVERNOR_POINTS_CHANGED*/, pPlayer->playerID, delta);
//
//   GetGovernorPointsSpent(this) @RVA 0x225EA90
//     遍历 Governor 列表累加 GetNumTitles() —— 纯读，无副作用。
//
// 因此本模块只做两件事：算出目标总量、把差值交给引擎。

#include "pch.h"
#include "HookAPI.h"
#include "GovernorAPI.h"
#include "../../Common/GameCoreResolve.h"

#include <cstdint>

namespace {

// ---------------------------------------------------------------------------
// 目标
// ---------------------------------------------------------------------------

// Player::Governors::ChangeGovernorPoints(int delta)  —— 引擎自己的增量入口。
// 序言 41 54 41 57 ... 在当前映像内唯一命中（已逐条扫 PE 复核）。
constexpr HookInfo kTargetChangePoints = HookName(
	"GameCore::Player::Governors::ChangeGovernorPoints",
	"48 89 5C 24 18 57 48 83 EC 30 48 8B D9 8B FA 48 8B 49 70");

// Player::Governors::GetGovernorPointsSpent()  —— 已花点数（纯读）。
constexpr HookInfo kTargetPointsSpent = HookName(
	"GameCore::Player::Governors::GetGovernorPointsSpent",
	"48 8B C4 48 89 58 20 55 41 54 41 57 48 8D 68 B8");

// Lua 参数 -> Player::Governors*
//   ScopedVirtualInstance<Lua::IPlayerGovernors, Player::Governors>::GetInstance(L, index, required)
//   required=false 时参数非法只返回 nullptr，不抛 Lua 错误。
constexpr HookInfo kTargetGovGetInstance = HookName(
	"GameCore::Lua::ScopedVirtualInstance<GameCore::Lua::IPlayerGovernors,"
	"GameCore::Player::Governors>::GetInstance",
	nullptr);   // 该族序言与 Unit/Plot 取参函数逐字节相同，AOB 无法区分

// 序言特征码（已停用，仅作注释保留）
constexpr uint8_t kSigChangePoints[8] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89 };
constexpr uint8_t kSigPointsSpent[8]  = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x20 };

// ---------------------------------------------------------------------------
// 结构体偏移
// ---------------------------------------------------------------------------
constexpr size_t OFF_GOV_TOTAL = 0x118;   // Governors -> int 总量（已花 + 可用）

HookAPI* g_api = nullptr;

typedef void  (__fastcall* ChangeGovernorPointsFn)(void* governors, int delta);
typedef int   (__fastcall* GetPointsSpentFn)(void* governors);
typedef void* (__fastcall* GovGetInstanceFn)(void* L, int index, bool required);

ChangeGovernorPointsFn g_changePoints = nullptr;
GetPointsSpentFn       g_pointsSpent = nullptr;
GovGetInstanceFn       g_govGetInstance = nullptr;

bool SymbolsReady() {
	return g_changePoints && g_pointsSpent && g_govGetInstance;
}

constexpr int kLuaTNumber = 3;

// 读第 index 个整数参数：类型不是 number 时返回 fallback（不抛 Lua 错误）。
int ReadIntArg(void* L, int index, int fallback) {
	if (!g_api) return fallback;
	if (g_api->LuaType(L, index) != kLuaTNumber) return fallback;
	return g_api->LuaToInteger(L, index);
}

// ---------------------------------------------------------------------------
// 核心
// ---------------------------------------------------------------------------
//
// 语义：把「可用点数」设成 targetAvailable，已花点数不动。
//   目标总量 = 已花 + targetAvailable
//   增量     = 目标总量 - 当前总量
// 增量交给引擎，由它负责 clamp / 提示 / 发事件。
bool SetAvailableCore(void* governors, int targetAvailable) {
	if (!governors) return false;
	if (targetAvailable < 0) targetAvailable = 0;

	const int spent = g_pointsSpent(governors);
	const int newTotal = spent + targetAvailable;
	const int oldTotal = *reinterpret_cast<int*>(static_cast<char*>(governors) + OFF_GOV_TOTAL);
	const int delta = newTotal - oldTotal;

	// delta == 0 也要走一次：引擎自己就是无条件发事件的，界面靠它刷新。
	g_changePoints(governors, delta);

	if (g_api) {
		g_api->Log("[GovernorAPI] SetAvailableGovPoints: spent=%d total %d -> %d (delta=%d)",
			spent, oldTotal, newTotal, delta);
	}
	return true;
}

// Player:GetGovernors():SetAvailableGovPoints(n) -> boolean
int lSetAvailableGovPoints(void* L) {
	if (!g_api) return 0;
	if (!SymbolsReady()) {
		g_api->LuaPushBoolean(L, false);
		return 1;
	}
	void* governors = g_govGetInstance(L, 1, /*required*/ false);
	const int value = ReadIntArg(L, 2, INT32_MIN);
	if (!governors || value == INT32_MIN) {
		g_api->LuaPushBoolean(L, false);
		return 1;
	}
	g_api->LuaPushBoolean(L, SetAvailableCore(governors, value));
	return 1;
}

} // namespace

// ---------------------------------------------------------------------------
// 入口 / 出口
// ---------------------------------------------------------------------------

void GovernorAPI_Init(HookAPI* api) {
	if (!api) return;
	g_api = api;

	g_changePoints = gc::ResolveVerified<ChangeGovernorPointsFn>(api, kHookImageGameCoreXP2,
		kTargetChangePoints, kSigChangePoints, sizeof(kSigChangePoints),
		"GovernorAPI/ChangeGovernorPoints");
	g_pointsSpent = gc::ResolveVerified<GetPointsSpentFn>(api, kHookImageGameCoreXP2,
		kTargetPointsSpent, kSigPointsSpent, sizeof(kSigPointsSpent),
		"GovernorAPI/GetGovernorPointsSpent");
	g_govGetInstance = gc::ResolveVerified<GovGetInstanceFn>(api, kHookImageGameCoreXP2,
		kTargetGovGetInstance, nullptr, 0, "GovernorAPI/Governors::GetInstance");

	if (!SymbolsReady()) {
		api->Log("%s", "[GovernorAPI] symbols incomplete - lua api returns false");
		return;
	}

	// 挂到原版 PlayerGovernors 元表上（引擎自己调 PushMethods 时由 Loader 追加）。
	static LuaMethodInfo s_methods[] = {
		{ &lSetAvailableGovPoints, "lSetAvailableGovPoints", "SetAvailableGovPoints" },
	};
	// sigType 必须与目标签名一致，选错不报错但参数会整体错位：
	//   IPlayerGovernors::PushMethods(this, L, stackOffset) —— 首参是对象（rdx=L），WithManager
	//   IMapPlot::PushMethods(L, stackOffset)               —— 首参就是 L（rcx），NoManager
	const HookApiPushMethodsResult pushed =
		api->PushLuaMethods("GameCore::Lua::IPlayerGovernors::PushMethods",
			kHookApiPushWithManager, s_methods, 1);

	api->Log("[GovernorAPI] active: SetAvailableGovPoints pushed=%d (spent=%p change=%p)",
		static_cast<int>(pushed), g_pointsSpent, g_changePoints);
}

void GovernorAPI_Shutdown() {
	g_changePoints = nullptr;
	g_pointsSpent = nullptr;
	g_govGetInstance = nullptr;
	g_api = nullptr;
}
