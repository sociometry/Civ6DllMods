// GreatWorkOptimizer.cpp
// 巨作分配优化 —— 从 CivilizationVI_CommunityExtension-1.3.0-beta 的
// GreatWorkOptimizer.cpp 移植到 Civ6DllLoader 插件框架。
//
// 相对上一版的改动（兼容性修复）：
//   1) 删除已从 HookAPI.h 移除的 RegisterGameCoreReady / RegisterGameCoreTeardown：
//      本工程是 GameCore 插件，OnLoad 执行时 GameCore 已加载完毕，代际回调没有意义；
//      退局时 Loader 按 owner 自动回滚钩子。
//   2) 取址从"裸 RVA + 序言特征码"改为 HookName(符号) + AOB 兜底（见 Common/GameCoreResolve.h）：
//      裸 RVA 在符号库与当前映像对不上时会被 Loader 原样透传，等于把钩子装到错误函数上。
//   3) 人类玩家判定改为按符号名解析 Player::Instance::IsHuman（直调用，不装钩），
//      解析失败时人类跳过分支自动失效
//      （退回原版全量重算），不会误伤 AI。
//   4) 所有日志从逐次执行降为"每 N 次/首次"打印，避免 O(G^3) 重算路径被日志拖慢。
//
// 逻辑本体（巨作槽位 FNV-1a 哈希、Execute 缓存跳过、bounded_knapsack 容量钳制）
// 逐条照抄参考实现；槽位布局已用当前 IDB 反汇编复核：
//   Execute：this+0x28 = Player::Instance*；player+0x6D0 = Player::Cities；
//            cities+0xD0 = 城市链表头（node[0]=City*, node[2]=next）；
//            city+0xD78 = GreatWorkInfo 数组，city+0xD88 = 容量，元素 0x20 字节；
//            info+0x00 = BuildingType，info+0x08 = int[2]/槽：[0]=GreatWorkListIndex，
//            [1] = theme（-1 = 空槽）；Building 定义 +0x1C8/+0x1D0 = Building_GreatWork 区间，
//            Building_GreatWork+0x24 = SlotNum。
//
// 线程模型：全部在游戏主线程（OnLoad 与 detour 都只在该线程执行），无锁。

#include "pch.h"
#include "HookAPI.h"
#include "GreatWorkOptimizer.h"
#include "../../Common/GameCoreResolve.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <intrin.h>

namespace {

// ---------------------------------------------------------------------------
// 目标描述：符号名为主路径，AOB 为兜底（每条 AOB 都在当前 IDB 内**唯一命中**）
// ---------------------------------------------------------------------------

// GameCore::AI::CultureAi_GreatWorks::Execute —— 虚函数，函数体很大（3374 字节），
// 前导 24 字节在当前版本唯一。
constexpr HookInfo kTargetExecute = HookName(
	"GameCore::AI::CultureAi_GreatWorks::Execute",
	"48 8B C4 48 89 48 08 55 53 57 48 8D A8 C8 FC FF FF 48 81 EC 20 04 00 00");

// bounded_knapsack<GreatWorkComboInfo*, lambda, lambda> —— 模板实例，符号名含 lambda 编号，
// 跨编译单元可能漂移，AOB 是主定位手段。
constexpr HookInfo kTargetKnapsack = HookName(
	"GameCore::BOUNDEDKNAPSACK_ALGORITHM::bounded_knapsack",
	"44 89 44 24 18 55 57 41 55 41 57 48 8D 6C 24 C8 48 81 EC 38 01 00 00");

// GameCore::Definitions::GetBuildingDefinitions —— 尾调用包装器，序言稳定。
constexpr HookInfo kTargetBuildingDefs = HookName(
	"GameCore::Definitions::GetBuildingDefinitions",
	"48 83 EC 28 E8 ?? ?? ?? ?? 48 05 20 04 00 00 48 83 C4 28 C3");

// GameCore::Player::Instance::IsHuman —— 纯函数，直调用（不装钩）。
// 序言：sub rsp,28h; mov edx,[rcx+0D8h]（+0xD8 = PlayerID，与遍历链的认知互证）。
// 注意：该前缀在当前映像里有 7 处同形命中（引擎把这段"取玩家配置"的样板内联了很多份），
// 所以**不带 AOB 兜底**——带上只会随机命中别的函数。符号名本身唯一，够用。
constexpr HookInfo kTargetPlayerIsHuman =
	HookName("GameCore::Player::Instance::IsHuman");

// 序言特征码【已移除】：入口可能被其它插件 hook/patch 改写，比对原始字节会误判。
// 相关参数仍保留在各 ResolveVerified 调用里以兼容接口，但不再被使用。
// 详见 Common/GameCoreResolve.h 顶部说明。
constexpr uint8_t kSigExecute[8] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x48, 0x08, 0x55 };
constexpr uint8_t kSigKnapsack[8] = { 0x44, 0x89, 0x44, 0x24, 0x18, 0x55, 0x57, 0x41 };

// ---------------------------------------------------------------------------
// 结构体偏移（反汇编复核，见文件头）
// ---------------------------------------------------------------------------
constexpr size_t OFF_PLAYER        = 0x28;    // CultureAi_GreatWorks -> Player::Instance
constexpr size_t OFF_PLAYER_ID     = 0xD8;    // Player::Instance -> PlayerID
constexpr size_t OFF_PLAYER_CITIES = 0x6D0;   // Player::Instance -> Player::Cities
constexpr size_t OFF_CITIES_HEAD   = 0xD0;    // Player::Cities  -> 城市链表头
constexpr size_t OFF_CITY_GWINFO   = 0xD78;   // City::Instance  -> GreatWorkInfo 数组
constexpr size_t OFF_CITY_GWCAP    = 0xD88;   // City::Instance  -> GreatWorkInfo 容量
constexpr size_t OFF_BUILDING_GWB  = 0x1C8;   // Definition::Building -> Building_GreatWork 起
constexpr size_t OFF_BUILDING_GWE  = 0x1D0;   // Definition::Building -> Building_GreatWork 止
constexpr size_t OFF_BGW_SLOTNUM   = 0x24;    // Definition::Building_GreatWork -> SlotNum
constexpr size_t GWINFO_SIZE       = 0x20;    // GreatWorkInfo 元素大小

HookAPI* g_api = nullptr;

// 统计（只用于日志，不参与行为）
std::atomic<uint64_t> g_executeCount{ 0 };
std::atomic<uint64_t> g_skipCount{ 0 };
std::atomic<uint64_t> g_humanSkips{ 0 };
std::atomic<uint64_t> g_totalExecuteMs{ 0 };

uint64_t g_cachedHash = 0;
bool     g_hasCache = false;

// ---------------------------------------------------------------------------
// 目标函数指针
// ---------------------------------------------------------------------------
typedef void* (__fastcall* GetBuildingDefinitionsFn)();
typedef void (__fastcall* ExecuteFn)(void* thisPtr);
typedef void* (__fastcall* KnapsackFn)(void* begin, void* end, unsigned int capacity,
	unsigned long long l1, unsigned long long l2);
typedef bool (__fastcall* PlayerIsHumanFn)(void* player);

ExecuteFn               g_origExecute = nullptr;
KnapsackFn              g_origKnapsack = nullptr;
GetBuildingDefinitionsFn g_getBuildingDefinitions = nullptr;   // 直调，非钩子
PlayerIsHumanFn          g_playerIsHuman = nullptr;            // 直调，非钩子

// ---------------------------------------------------------------------------
// FNV-1a 64（照抄参考实现）
// ---------------------------------------------------------------------------
inline uint64_t fnv1a64(uint64_t hash, const void* data, size_t len) {
	const uint8_t* p = static_cast<const uint8_t*>(data);
	for (size_t i = 0; i < len; ++i) {
		hash ^= p[i];
		hash *= 0x100000001b3ULL;
	}
	return hash;
}

// 复刻 Execute 第一阶段遍历，只哈希非空槽位的（位置, 内容）。
uint64_t ComputeGreatWorkHash(void* gwThis, int* outCount) {
	uint64_t h = 0xcbf29ce484222325ULL;
	int count = 0;

	void* player = *reinterpret_cast<void**>(static_cast<char*>(gwThis) + OFF_PLAYER);
	if (!player) return h;
	void* cities = *reinterpret_cast<void**>(static_cast<char*>(player) + OFF_PLAYER_CITIES);
	if (!cities) return h;
	void** cityNode = *reinterpret_cast<void***>(static_cast<char*>(cities) + OFF_CITIES_HEAD);

	void* defs = g_getBuildingDefinitions ? g_getBuildingDefinitions() : nullptr;

	while (cityNode) {
		void* city = cityNode[0];
		if (city) {
			char* info = *reinterpret_cast<char**>(static_cast<char*>(city) + OFF_CITY_GWINFO);
			const int capacity = *reinterpret_cast<int*>(static_cast<char*>(city) + OFF_CITY_GWCAP);
			if (info && capacity > 0 && capacity < 4096) {
				for (int i = 0; i < capacity; ++i) {
					char* gwInfo = info + static_cast<ptrdiff_t>(GWINFO_SIZE) * i;
					const int buildingType = *reinterpret_cast<int*>(gwInfo);

					if (!defs) continue;
					void** begin = *reinterpret_cast<void***>(defs);
					void** end = *reinterpret_cast<void***>(static_cast<char*>(defs) + 8);
					if (!begin || !end || end < begin) continue;
					const size_t defCount = static_cast<size_t>(end - begin);
					if (static_cast<size_t>(buildingType) >= defCount) continue;
					void* building = begin[buildingType];
					if (!building) continue;

					char** gwBegin = *reinterpret_cast<char***>(static_cast<char*>(building) + OFF_BUILDING_GWB);
					char** gwEnd = *reinterpret_cast<char***>(static_cast<char*>(building) + OFF_BUILDING_GWE);
					if (!gwBegin || !gwEnd || gwEnd < gwBegin) continue;
					for (char** p = gwBegin; p != gwEnd; ++p) {
						void* bd = *p;
						if (!bd) continue;
						const int slotNum = *reinterpret_cast<int*>(static_cast<char*>(bd) + OFF_BGW_SLOTNUM);
						if (slotNum <= 0 || slotNum > 16) continue;
						int* slots = *reinterpret_cast<int**>(gwInfo + 0x08);
						if (!slots) continue;
						for (int s = 0; s < slotNum; ++s) {
							// 非空槽位：[0] = 巨作列表序号，[1] = theme（-1 = 空）
							if (slots[2 * s + 1] != -1) {
								h = fnv1a64(h, &i, sizeof(i));
								h = fnv1a64(h, &s, sizeof(s));
								h = fnv1a64(h, &slots[2 * s], 2 * sizeof(int));
								++count;
							}
						}
					}
				}
			}
		}
		cityNode = reinterpret_cast<void**>(cityNode[2]);
	}
	if (outCount) *outCount = count;
	return h;
}

int GetPlayerID(void* gwThis) {
	void* player = *reinterpret_cast<void**>(static_cast<char*>(gwThis) + OFF_PLAYER);
	if (!player) return -1;
	return *reinterpret_cast<int*>(static_cast<char*>(player) + OFF_PLAYER_ID);
}

// 人类玩家判定：player +0x28 直接是 Player::Instance*，成员调用 IsHuman 即可
// （内部自行读 +0xD8 的 PlayerID 查配置）。IsHuman 不可用时恒 false = 不跳过。
bool IsHumanPlayer(void* gwThis) {
	if (!g_playerIsHuman) return false;
	void* player = *reinterpret_cast<void**>(static_cast<char*>(gwThis) + OFF_PLAYER);
	if (!player) return false;
	return g_playerIsHuman(player);
}

// ---------------------------------------------------------------------------
// detour
// ---------------------------------------------------------------------------

// bounded_knapsack：钳制 capacity 到 3（最大组合数为 3，capacity>3 的 DP 列恒等）。
void* __fastcall Hook_Knapsack(void* begin, void* end, unsigned int capacity,
	unsigned long long l1, unsigned long long l2) {
	KnapsackFn orig = g_origKnapsack;
	if (!orig) return nullptr;
	if (capacity > 3) capacity = 3;
	return orig(begin, end, capacity, l1, l2);
}

void __fastcall Hook_Execute(void* thisPtr) {
	ExecuteFn orig = g_origExecute;
	if (!orig) return;   // 已被回滚：直通空操作

	// 人类玩家的文化 AI 重算无意义（巨作摆放由玩家手动控制），整次跳过。
	if (IsHumanPlayer(thisPtr)) {
		++g_humanSkips;
		return;
	}

	int gwCount = 0;
	const uint64_t h = ComputeGreatWorkHash(thisPtr, &gwCount);

	if (g_hasCache && h == g_cachedHash) {
		++g_skipCount;   // 巨作未变：跳过整个 O(G^3) 重算
		return;
	}
	g_cachedHash = h;
	g_hasCache = true;

	orig(thisPtr);
	++g_executeCount;

	// 日志降噪：首次 + 每 32 次打一条（O(G^3) 路径不能被日志拖慢）。
	const uint64_t n = g_executeCount.load();
	if (g_api && (n == 1 || (n % 32) == 0)) {
		g_api->Log("[GreatWorkOptimizer] player=%d gwCount=%d exec=%llu cacheSkip=%llu humanSkip=%llu",
			GetPlayerID(thisPtr), gwCount,
			static_cast<unsigned long long>(n),
			static_cast<unsigned long long>(g_skipCount.load()),
			static_cast<unsigned long long>(g_humanSkips.load()));
	}
}

} // namespace

// ---------------------------------------------------------------------------
// 入口 / 出口
// ---------------------------------------------------------------------------

void GreatWorkOptimizer_Init(HookAPI* api) {
	if (!api) return;
	g_api = api;

	// 直调用符号（不装钩）：拿不到就降级，不阻塞其余功能。
	// 注意：这里不做序言校验 —— 入口可能已被其它插件 hook/patch 改写，
	// 比对原始字节会误判成"游戏更新了"。详见 Common/GameCoreResolve.h 顶部说明。
	g_getBuildingDefinitions = gc::Resolve<GetBuildingDefinitionsFn>(
		api, kHookImageGameCoreXP2, kTargetBuildingDefs, 16);

	// 人类玩家判定：只靠符号名（该符号唯一）。
	g_playerIsHuman = gc::Resolve<PlayerIsHumanFn>(api, kHookImageGameCoreXP2,
		kTargetPlayerIsHuman, 16);

	unsigned installed = 0;

	// Execute（虚函数）
	if (void* target = gc::ResolveVerified<void*>(api, kHookImageGameCoreXP2, kTargetExecute,
			kSigExecute, sizeof(kSigExecute), "GreatWorkOptimizer/Execute")) {
		if (api->InstallHook(target, reinterpret_cast<void*>(&Hook_Execute),
				reinterpret_cast<void**>(&g_origExecute))) {
			++installed;
		} else {
			api->Log("%s", "[GreatWorkOptimizer] InstallHook(Execute) failed");
		}
	}

	// bounded_knapsack
	if (void* target = gc::ResolveVerified<void*>(api, kHookImageGameCoreXP2, kTargetKnapsack,
			kSigKnapsack, sizeof(kSigKnapsack), "GreatWorkOptimizer/knapsack")) {
		if (api->InstallHook(target, reinterpret_cast<void*>(&Hook_Knapsack),
				reinterpret_cast<void**>(&g_origKnapsack))) {
			++installed;
		} else {
			api->Log("%s", "[GreatWorkOptimizer] InstallHook(bounded_knapsack) failed");
		}
	}

	g_cachedHash = 0;
	g_hasCache = false;

	api->Log("[GreatWorkOptimizer] installed %u/2 hooks; buildingDefs=%s humanSkip=%s",
		installed,
		g_getBuildingDefinitions ? "ok" : "missing",
		g_playerIsHuman ? "on" : "off");
}

void GreatWorkOptimizer_Shutdown() {
	// 钩子由 Loader 按 owner 统一回滚；这里只断引用，避免 detour 再跳回已卸载的模块。
	g_origExecute = nullptr;
	g_origKnapsack = nullptr;
	g_getBuildingDefinitions = nullptr;
	g_playerIsHuman = nullptr;
	g_hasCache = false;
	g_api = nullptr;
}
