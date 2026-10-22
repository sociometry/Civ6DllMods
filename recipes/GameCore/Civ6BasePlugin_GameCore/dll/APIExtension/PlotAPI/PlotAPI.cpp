// PlotAPI.cpp
// Plot:ChangeAppeal(n) 的实现：账本 + 读拦截。
//
// 引擎侧事实（IDA 反编译 / vtable 实读核实，GameCore_XP2_FinalRelease.dll）：
//
//   Plot vtable @ 0x172940450（符号 ??_7Instance@Plot@Map@GameCore@@6B@）
//     槽 5  (+0x28) = Map::Plot::Instance::GetAppeal          ← 本模块的拦截点
//     槽 6  (+0x30) = Map::Plot::Instance::GetCachedAppeal    （返回 +0x4A 的 int16）
//
//   Map::Plot::Instance::GetAppeal(Plot*) @RVA 0x1DF030
//     if (this->vtable[0x30]()->byte[0x1C] != 0xFF) return *(int16*)(this + 0x4A);  // 缓存
//     return Rules::Appeal::Get()->GetAppeal(this);                                  // 现算
//
//   Lua::IMapPlot::lGetAppeal @RVA 0x1F9CE40
//     plot = ScopedInstance<IMapPlot, Plot::Instance>::GetInstance(L, 1, true);
//     return Reporting::Notification::Instance::_vcall_40(plot);   // 即 mov rax,[rcx]; jmp [rax+28h]
//
// 也就是说：Lua 读魅力 -> 跳板 -> vtable+0x28 -> 本模块 detour。
// 直接 hook 函数体（而不是那个 6 字节跳板）能同时覆盖引擎内部的虚调用。
//
// 为什么增量不写进真实数据：
//   魅力是现算值，没有"可写的真实字段"；缓存字段只在特定条件下生效且会被重算覆盖。
//   写账本 + 读时叠加既不污染存档，也不会被重算冲掉。

#include "pch.h"
#include "HookAPI.h"
#include "PlotAPI.h"
#include "../../Common/GameCoreResolve.h"

#include <cstdint>
#include <unordered_map>

namespace {

// ---------------------------------------------------------------------------
// 目标
// ---------------------------------------------------------------------------

// Map::Plot::Instance::GetAppeal —— 虚函数体本体（vtable 槽 5）。
// 序言 push rbx; sub rsp,20h; mov rax,[rcx] ... 在映像内有多处同形，
// 因此只走符号名，不带 AOB 兜底。
constexpr HookInfo kTargetGetAppeal = HookName(
	"GameCore::Map::Plot::Instance::GetAppeal",
	nullptr);

// Lua 参数 -> Plot::Instance*
//   ScopedInstance<Lua::IMapPlot, Map::Plot::Instance>::GetInstance(L, index, required)
constexpr HookInfo kTargetPlotGetInstance = HookName(
	"GameCore::Lua::ScopedInstance<GameCore::Lua::IMapPlot,"
	"GameCore::Map::Plot::Instance>::GetInstance",
	nullptr);   // 与 Unit / Governors 取参函数序言逐字节相同，AOB 无法区分

// Map::Plot::Instance::GetIndex() —— 地块索引不是字段，是现算的：
//   index = (int16)(this+0x28) + (int16)(this+0x2A) * EditMap()->width(+0x28)
// 所以账本的键必须调它取，不能直读某个偏移。
constexpr HookInfo kTargetGetIndex = HookName(
	"GameCore::Map::Plot::Instance::GetIndex",
	"48 89 5C 24 08 57 48 83 EC 20 0F BF 59 2A 0F BF 79 28");

HookAPI* g_api = nullptr;

typedef int   (__fastcall* GetAppealFn)(void* plot);
typedef void* (__fastcall* PlotGetInstanceFn)(void* L, int index, bool required);
typedef int   (__fastcall* GetIndexFn)(void* plot);

GetAppealFn         g_origGetAppeal = nullptr;
PlotGetInstanceFn   g_plotGetInstance = nullptr;
GetIndexFn          g_getIndex = nullptr;

// 账本：地块索引 -> 累计魅力增量。
//
// 用 unordered_map 而不是 std::map：这里是热路径（每帧、每个地块都会被问到），
// 只做插入 / 查找，不需要有序遍历。
std::unordered_map<int, int> g_appealDelta;

constexpr int kLuaTNumber = 3;

bool SymbolsReady() {
	return g_origGetAppeal && g_plotGetInstance && g_getIndex;
}

int ReadIntArg(void* L, int index, int fallback) {
	if (!g_api) return fallback;
	if (g_api->LuaType(L, index) != kLuaTNumber) return fallback;
	return g_api->LuaToInteger(L, index);
}

// ---------------------------------------------------------------------------
// 读拦截
// ---------------------------------------------------------------------------
//
// 注意：detour 里不能调任何会抛 Lua 错误的东西，也不能长时间持锁。
// 这里只有一次哈希查找，没有锁（全在游戏主线程）。
int __fastcall Hook_GetAppeal(void* plot) {
	const int original = g_origGetAppeal(plot);
	if (!plot || g_appealDelta.empty()) return original;

	// GetIndex 是现算的（y * mapWidth + x），不是可直读的字段，必须调函数。
	const auto it = g_appealDelta.find(g_getIndex(plot));
	return it == g_appealDelta.end() ? original : original + it->second;
}

// Plot:ChangeAppeal(n) -> boolean
int lChangeAppeal(void* L) {
	if (!g_api) return 0;
	if (!SymbolsReady()) {
		g_api->LuaPushBoolean(L, false);
		return 1;
	}
	void* plot = g_plotGetInstance(L, 1, /*required*/ false);
	const int delta = ReadIntArg(L, 2, 0);
	if (!plot) {
		g_api->LuaPushBoolean(L, false);
		return 1;
	}

	const int plotIndex = g_getIndex(plot);
	int& acc = g_appealDelta[plotIndex];
	acc += delta;
	const int newTotal = acc;
	if (newTotal == 0) {
		// 归零就删掉条目，避免账本随"加了又减回去"无限膨胀。
		g_appealDelta.erase(plotIndex);
	}

	if (g_api) {
		g_api->Log("[PlotAPI] ChangeAppeal: plot=%d delta=%d -> +%d", plotIndex, delta, newTotal);
	}
	g_api->LuaPushBoolean(L, true);
	return 1;
}

} // namespace

// ---------------------------------------------------------------------------
// 入口 / 出口
// ---------------------------------------------------------------------------

void PlotAPI_Init(HookAPI* api) {
	if (!api) return;
	g_api = api;

	g_appealDelta.clear();

	g_plotGetInstance = gc::ResolveVerified<PlotGetInstanceFn>(api, kHookImageGameCoreXP2,
		kTargetPlotGetInstance, nullptr, 0, "PlotAPI/Plot::GetInstance");

	g_getIndex = gc::ResolveVerified<GetIndexFn>(api, kHookImageGameCoreXP2,
		kTargetGetIndex, nullptr, 0, "PlotAPI/Plot::GetIndex");

	void* target = gc::ResolveVerified<void*>(api, kHookImageGameCoreXP2,
		kTargetGetAppeal, nullptr, 0, "PlotAPI/Plot::GetAppeal");
	if (!g_plotGetInstance || !g_getIndex || !target) {
		api->Log("%s", "[PlotAPI] symbols incomplete - lua api returns false");
		return;
	}

	if (!api->InstallHook(target, reinterpret_cast<void*>(&Hook_GetAppeal),
			reinterpret_cast<void**>(&g_origGetAppeal))) {
		api->Log("%s", "[PlotAPI] InstallHook(Plot::GetAppeal) failed");
		return;
	}

	static LuaMethodInfo s_methods[] = {
		{ &lChangeAppeal, "lChangeAppeal", "ChangeAppeal" },
	};
	// IMapPlot::PushMethods(L, stackOffset)：首参就是 L（rcx），NoManager 形态。
	const HookApiPushMethodsResult pushed =
		api->PushLuaMethods("GameCore::Lua::IMapPlot::PushMethods",
			kHookApiPushNoManager, s_methods, 1);

	api->Log("[PlotAPI] active: ChangeAppeal pushed=%d (appeal hooked at %p)",
		static_cast<int>(pushed), target);
}

void PlotAPI_Shutdown() {
	// 账本随插件卸载清空：地块索引只在同一张地图内有意义。
	g_appealDelta.clear();
	g_origGetAppeal = nullptr;
	g_plotGetInstance = nullptr;
	g_getIndex = nullptr;
	g_api = nullptr;
}
