// GreatWorkModifierAttach.cpp
// 普通巨作创建后没有挂上 Modifier 的修复。
//
// ---------------------------------------------------------------------------
// 现象
// ---------------------------------------------------------------------------
// 伟人创作出来的巨作（著作 / 艺术品 / 文物 / 遗物）不会触发 Database 里
// 挂在该 GreatWork 上的 Modifier；而 Product（大商人随机产品）与 HeroEpic
// 产出的巨作正常。
//
// ---------------------------------------------------------------------------
// 根因（IDA 反汇编实证）
// ---------------------------------------------------------------------------
// 给巨作挂 Modifier 的唯一入口是
//   GameEffects::GameEventHandler::OnGreatWorkAdded_AttachGameEffects(City*, GreatWorkTypes)
// 它在 GameEffects::GameEventHandler 构造函数里订阅到
//   Game::Signals + 0x19B0
// 这条 ProtoSignal 上（同构造函数里 [this+0x19C8] 绑的是 Remove 版）。
//
// 该信号的发射方（对全库 75 个 ProtoSignal::Dispatch 调用点逐一回溯偏移统计）：
//   +0x19B0 只有 2 处
//     · GameCore::Player::Operations::Manager::Process        （移动 / 交换巨作）
//     · GameCore::Game::Economic::Manager::GrantRandomProduct （Product）
//   +0x240  2 处（AddGreatWork / CheckForThemedBuilding 发的另一条信号，与挂 Modifier 无关）
//
// 也就是说：伟人创作这条【普通路径】从 AddGreatWorkToTile → AddGreatWorkToBuilding
// 一路走下来，只发了 Reporting::Event::Send(GREAT_WORK_CREATED)（给 UI 用的上报事件），
// 从未发过 +0x19B0 这条 GameCore 内部信号 —— 所以 GameEffects 侧根本不知道有新巨作，
// Modifier 自然挂不上。
//
// Product 之所以正常，正是因为 GrantRandomProduct 自己补了这一次 Dispatch。
//
// ---------------------------------------------------------------------------
// 修法
// ---------------------------------------------------------------------------
// hook GameCore::City::Buildings::AddGreatWorkToBuilding ——
// 伟人创作落槽的唯一出口（唯一调用者是 AddGreatWorkToTile），
// 在原函数成功返回后补一次 Dispatch(+0x19B0, city, greatWorkType)。
//
// 为什么只钩这一个、不动 Buildings::AddGreatWork：
//   AddGreatWork 的调用者是 MoveCapital / FindandMovetoNewCapital /
//   RelocateBuildingGreatWorks / ReceiveGreatWork / Player::Cities::AddGreatWork，
//   全是【搬移既有巨作】或【窃取转手】，不是"新创作"。这些路径上补 Added 信号
//   等于给新城市重复挂 Modifier，而旧城市那份从来没有对应的 Removed 信号来解除
//   （+0x19C8 全库无人发射），会越搬越多。所以那一条保持原样。
//
// 为什么不钩 AddGreatWorkToSlot：
//   Product 与 Process 已经在自己的调用点补过 Dispatch，AddGreatWorkToSlot 是
//   公共落点，钩它会与那两条重复挂载，造成双倍加成。
//
// ---------------------------------------------------------------------------
// 一条必须写下来的坑
// ---------------------------------------------------------------------------
// Dispatch 的模板实例会因为机器码完全一致被链接器 ICF 合并（+0x19B0 与 +0x240
// 的 Dispatch 头 29 字节相同，是同一份代码的两个符号）。它的 ABI 只跟形参的
// 【寄存器和个数】有关，与具体类型无关：
//     rcx = ProtoSignal*
//     rdx = 第 1 实参（这里统一是 City::Instance const*）
//     r8d = 第 2 实参（这里统一是 GreatWorkTypes）
// 所以可以直接按 (signal, city, greatWorkType) 声明调用，不必区分类型名。
//
// 线程模型：OnLoad 与 detour 都在游戏主线程。

#include "pch.h"
#include "HookAPI.h"
#include "GreatWorkModifierAttach.h"
#include "../../Common/GameCoreResolve.h"

#include <cstdint>

namespace {

// ---------------------------------------------------------------------------
// 目标
// ---------------------------------------------------------------------------

// Buildings::AddGreatWorkToBuilding（public，RVA 0x109890）
//   GreatWorkTypes __thiscall (Buildings*, GreatPersonIndividualTypes, PlotCoord, BuildingTypes*)
//   rcx = Buildings*，edx = individualType，r8 = PlotCoord（8 字节打包），r9 = BuildingTypes*
//   返回：放进去的 GreatWorkTypes；-1 = 没找到能放的槽。
constexpr HookInfo kTargetAddToBuilding = HookName(
	"GameCore::City::Buildings::AddGreatWorkToBuilding",
	nullptr);   // 该序言在库里有两处命中（0x109890 与 0x2ECB00），不能做 AOB 兜底

// ProtoSignal<void(City::Instance const*, GreatWorkTypes)>::Dispatch（RVA 0xDA700）
//
// 注意：库里这条符号登记的名字是
//   ?Dispatch@?$ProtoSignal@$A6AXW4PlayerTypes@GameCore@@PEBVInstance@City@2@@Z...
// 也就是 void(PlayerTypes, City const*) 那一份 —— 因为 ICF 把参数形状相同的
//   Dispatch 实例合并成了同一份机器码，符号名只是「谁先被链接到」的结果。
// 真正的实参顺序由发射点决定（见文件头）：rdx = City*，r8d = GreatWorkTypes。
// 因此这里按 (signal, city, greatWorkType) 调用，与实际发射点逐字节一致。
//
// AOB 同样因 ICF 有两处命中，只走符号名。
constexpr HookInfo kTargetDispatch = HookName(
	"?Dispatch@?$ProtoSignal@$$A6AXW4PlayerTypes@GameCore@@PEBVInstance@City@2@@Z"
	"U?$CollectorDefault@X@SignalBase@Message@2@@SignalBase@Message@GameCore@@QEAAX"
	"W4PlayerTypes@4@PEBVInstance@City@4@@Z",
	nullptr);

// GameCore::Game::Instance::Edit()（RVA 0x951550）—— 返回 ms_pkCurrentGame
constexpr HookInfo kTargetGameEdit = HookName(
	"GameCore::Game::Instance::Edit",
	"48 8B 05 ?? ?? ?? ?? C3");

// ---------------------------------------------------------------------------
// 偏移（IDA 实证）
// ---------------------------------------------------------------------------
constexpr size_t OFF_GAME_SIGNALS     = 0x280;   // Game::Instance -> Signals
constexpr size_t OFF_SIGNAL_GW_ADDED  = 0x19B0;  // Signals -> ProtoSignal<GreatWorkAdded>
constexpr size_t OFF_BUILDINGS_CITY   = 0x000;   // Buildings -> City::Instance*（m_pCity）

// ---------------------------------------------------------------------------
// 函数指针
// ---------------------------------------------------------------------------
typedef int  (__fastcall* AddGreatWorkToBuildingFn)(
	void* buildings, int individualType, uint64_t plotCoord, int* outBuildType);
typedef void (__fastcall* SignalDispatchFn)(void* signal, const void* city, int greatWorkType);
typedef void*(__fastcall* GameEditFn)();

HookAPI* g_api = nullptr;
AddGreatWorkToBuildingFn g_origAddToBuilding = nullptr;
SignalDispatchFn         g_dispatch = nullptr;
GameEditFn               g_gameEdit = nullptr;

// ---------------------------------------------------------------------------
// 发一次 GreatWorkAdded 信号
// ---------------------------------------------------------------------------
void DoDispatchGreatWorkAdded(void* buildings, int greatWorkType) {
	if (!buildings || greatWorkType < 0) return;
	if (!g_dispatch || !g_gameEdit) return;

	const void* city = *reinterpret_cast<void* const*>(
		static_cast<const char*>(buildings) + OFF_BUILDINGS_CITY);
	if (!city) return;

	void* game = g_gameEdit();
	if (!game) return;

	char* signals = *reinterpret_cast<char**>(static_cast<char*>(game) + OFF_GAME_SIGNALS);
	if (!signals) return;

	g_dispatch(signals + OFF_SIGNAL_GW_ADDED, city, greatWorkType);
}

// ---------------------------------------------------------------------------
// Detour
// ---------------------------------------------------------------------------
int __fastcall Hook_AddGreatWorkToBuilding(
	void* buildings, int individualType, uint64_t plotCoord, int* outBuildType) {
	if (!g_origAddToBuilding) return -1;

	const int added = g_origAddToBuilding(buildings, individualType, plotCoord, outBuildType);
	if (added >= 0) {
		DoDispatchGreatWorkAdded(buildings, added);
	}
	return added;
}

} // namespace

void GreatWorkModifierAttach_Init(HookAPI* api) {
	if (!api) return;
	g_api = api;
	g_origAddToBuilding = nullptr;
	g_dispatch = nullptr;
	g_gameEdit = nullptr;

	g_dispatch = gc::ResolveVerified<SignalDispatchFn>(api, kHookImageGameCoreXP2,
		kTargetDispatch, nullptr, 0, "GreatWorkModifierAttach/Dispatch");
	g_gameEdit = gc::ResolveVerified<GameEditFn>(api, kHookImageGameCoreXP2,
		kTargetGameEdit, nullptr, 0, "GreatWorkModifierAttach/GameEdit");

	if (!g_dispatch || !g_gameEdit) {
		api->Log("%s",
			"[GreatWorkModifierAttach] Dispatch / Game::Instance::Edit unresolved - skipped");
		return;
	}

	void* target = gc::ResolveVerified<void*>(api, kHookImageGameCoreXP2,
		kTargetAddToBuilding, nullptr, 0, "GreatWorkModifierAttach/AddGreatWorkToBuilding");
	if (!target) {
		api->Log("%s", "[GreatWorkModifierAttach] AddGreatWorkToBuilding unresolved - skipped");
		return;
	}

	if (!api->InstallHook(target, reinterpret_cast<void*>(&Hook_AddGreatWorkToBuilding),
			reinterpret_cast<void**>(&g_origAddToBuilding))) {
		api->Log("%s", "[GreatWorkModifierAttach] InstallHook(AddGreatWorkToBuilding) failed");
		return;
	}

	api->Log("%s",
		"[GreatWorkModifierAttach] active: great works from Great Person now fire GreatWorkAdded");
}

// 公开出口：供 GreatWorkManager 接口在「确实新增了一件巨作」后调用。
//
// 参数口径与内部版本一致：buildings 为该巨作的落点，greatWorkType 是
// Definition::GreatWork 的索引（不是 listIndex）。
// 模块未就绪时静默返回 —— 调用方不需要判空，也不会因此中断主流程。
void GreatWorkModifierAttach_NotifyAdded(void* buildings, int greatWorkType) {
	DoDispatchGreatWorkAdded(buildings, greatWorkType);
}

void GreatWorkModifierAttach_Shutdown() {
	g_origAddToBuilding = nullptr;
	g_dispatch = nullptr;
	g_gameEdit = nullptr;
	g_api = nullptr;
}