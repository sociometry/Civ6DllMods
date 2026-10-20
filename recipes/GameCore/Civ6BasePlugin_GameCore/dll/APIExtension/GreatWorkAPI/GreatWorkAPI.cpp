// GreatWorkAPI.cpp
// 巨作添加整合接口（APIExtension 分类）：两个开箱即用的 Lua 接口，Lua 侧无需手工组合。
//
// 相对上一版的改动（兼容性修复）：
//   1) EditPlayer -> GetPlayer（HookAPI.h 中的现名）；
//   2) 删除 RegisterGameCoreReady / RegisterGameCoreTeardown，改为 OnLoad 内直接解析；
//   3) 全部取址改为符号名（含修饰名）+ AOB 兜底，不再依赖裸 RVA；
//   4) Lua 侧参数从 LuaCheckInteger（会抛 Lua 错误）改为 LuaType 判断 + OptInteger，
//      非法参数返回 false 而不是让脚本报错；
//   5) 同时注册到 gameplay 与 UI 两个脚本环境（CE 版只有 gameplay）。
//
// 已用当前版本 IDB 反编译核实的调用链：
//   GameCore::Game::Culture::Edit                     () -> Culture&（static）
//   GameCore::Game::Culture::FindOrAddGreatWork       (culture, greatWorkIndex) -> listIndex
//   GameCore::Game::Culture::SetGreatWorkPlayer       (culture, listIndex, playerId)
//   GameCore::City::Buildings::CanAddGreatWork        (buildings, listIndex, &buildingType) -> bool
//   GameCore::City::Buildings::AddGreatWork           (buildings, listIndex) 自动找槽
//   GameCore::City::Buildings::AddGreatWorkToSlot     (buildings, listIndex, buildingType, slotIndex)
//   GameCore::Player::Districts_Units_Cities::Find    (cities, cityID) -> City*
//
// 内存布局（汇编核实）：
//   Player::Instance +0x6D0 = Player::Cities
//   Player::Cities   +0xD0  = 首都城市链表头；node[0]=City*，node[2]=next
//   City::Instance   +0xCB0 = Buildings 对象本体（引擎内部一律 lea rcx,[city+0CB0h] 后当
//                             Buildings* 传；+0xCA0 是它的 FAutoVariable 包装器）

#include "pch.h"
#include "HookAPI.h"
#include "GreatWorkAPI.h"
#include "../../Common/GameCoreResolve.h"

#include <cstdint>

namespace {

// ---------------------------------------------------------------------------
// 目标描述（AOB 均在当前 IDB 内唯一命中）
// ---------------------------------------------------------------------------
// Culture::Edit 的样板前缀（push rbx; sub rsp,20h; mov ebx,cs:xxx; call ...）在当前映像里
// 有 47 处同形命中，AOB 兜底会随机命中；它的修饰名唯一，因此只用符号名。
constexpr HookInfo kTargetCultureEdit = HookName("GameCore::Game::Culture::Edit");

constexpr HookInfo kTargetFindOrAddGreatWork = HookName(
	"GameCore::Game::Culture::FindOrAddGreatWork",
	"48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 4C 8B 81 20 01 00 00");

constexpr HookInfo kTargetSetGreatWorkPlayer = HookName(
	"GameCore::Game::Culture::SetGreatWorkPlayer",
	"48 89 5C 24 08 57 48 83 EC 20 48 81 C1 08 01 00 00 8B DA");

constexpr HookInfo kTargetCanAddGreatWork = HookName(
	"GameCore::City::Buildings::CanAddGreatWork",
	"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 54 41 56 41 57 48 83 EC 20 4D 8B E0 8B EA 48 8B D9");

constexpr HookInfo kTargetAddGreatWork = HookName(
	"GameCore::City::Buildings::AddGreatWork",
	"48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 54 41 55 41 56 41 57 48 83 EC 20 44 8B FA");

constexpr HookInfo kTargetAddGreatWorkToSlot = HookName(
	"GameCore::City::Buildings::AddGreatWorkToSlot",
	"48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 4C 8B 91 C8 00 00 00");

constexpr HookInfo kTargetCitiesFind = HookName(
	"GameCore::Player::Districts_Units_Cities::Find",
	"83 FA FF 74 3D 4C 8B 91 98 00 00 00 48 8B 81 A0 00 00 00 49 2B C2");

// 序言特征码（当前仅作注释保留，不参与校验）
constexpr uint8_t kSigCultureEdit[6]      = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20 };
constexpr uint8_t kSigFindOrAdd[8]        = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74 };
constexpr uint8_t kSigSetPlayer[8]        = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83 };
constexpr uint8_t kSigCanAdd[8]           = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C };
constexpr uint8_t kSigAddGreatWork[8]     = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C };
constexpr uint8_t kSigAddToSlot[8]        = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74 };
constexpr uint8_t kSigCitiesFind[8]       = { 0x83, 0xFA, 0xFF, 0x74, 0x3D, 0x4C, 0x8B, 0x91 };

// ---------------------------------------------------------------------------
// 结构体偏移
// ---------------------------------------------------------------------------
constexpr size_t OFF_PLAYER_CITIES = 0x6D0;   // Player::Instance -> Player::Cities
constexpr size_t OFF_CITIES_HEAD   = 0xD0;    // Player::Cities  -> 城市链表头
constexpr size_t OFF_CITY_BUILDINGS = 0xCB0;  // City::Instance -> Buildings

// Culture 的 greatWorks 列表（eastl::vector<GreatWork>，元素 24 字节）
constexpr size_t OFF_CULTURE_GW_BEGIN = 0x118;
constexpr size_t OFF_CULTURE_GW_END   = 0x120;
constexpr size_t GW_ENTRY_SIZE        = 0x18;
constexpr size_t OFF_GW_TYPE          = 0x00;   // greatWorkType
constexpr size_t OFF_GW_PLAYER        = 0x04;   // player（-1 = 未分配）

// Buildings 内的巨作槽记录
constexpr size_t OFF_BLD_GW_DATA    = 0x0C8;
constexpr size_t OFF_BLD_GW_COUNT   = 0x0D8;
constexpr size_t BLD_GWINFO_SIZE    = 0x20;
constexpr size_t OFF_GWINFO_TYPE    = 0x00;
constexpr size_t OFF_GWINFO_SLOTS   = 0x08;
constexpr size_t OFF_GWINFO_NUM     = 0x18;
constexpr size_t GWSLOT_SIZE        = 0x08;
constexpr size_t OFF_GWSLOT_INDEX   = 0x04;
constexpr int    kEmptyGwSlot       = -1;
constexpr int    kMaxSanityCount    = 4096;

HookAPI* g_api = nullptr;

typedef void* (__cdecl*    CultureEditFn)();
typedef int   (__fastcall* FindOrAddGreatWorkFn)(void* culture, unsigned int greatWorkIndex);
typedef void  (__fastcall* SetGreatWorkPlayerFn)(void* culture, int listIndex, int playerId);
typedef bool  (__fastcall* CanAddGreatWorkFn)(void* buildings, int listIndex, int* outBuildingType);
typedef void  (__fastcall* AddGreatWorkFn)(void* buildings, int listIndex);
typedef void  (__fastcall* AddGreatWorkToSlotFn)(void* buildings, int listIndex, int buildingType, int slotIndex);
typedef void* (__fastcall* CitiesFindFn)(void* cities, int cityId);
typedef int   (__fastcall* FindOrAddGreatWorkOrigFn)(void* culture, unsigned int greatWorkType);

CultureEditFn           g_cultureEdit = nullptr;
FindOrAddGreatWorkFn    g_findOrAddGreatWork = nullptr;
SetGreatWorkPlayerFn    g_setGreatWorkPlayer = nullptr;
CanAddGreatWorkFn       g_canAddGreatWork = nullptr;
AddGreatWorkFn          g_addGreatWork = nullptr;
// 注意：g_addGreatWorkToSlot 目前【不用于放置】，只保留解析与 SymbolsReady 判定，
// 避免改变对外可用性。放置统一走 g_addGreatWork（内部有 IsValidObjectForSlot 校验）。
AddGreatWorkToSlotFn    g_addGreatWorkToSlot = nullptr;
CitiesFindFn            g_citiesFind = nullptr;
FindOrAddGreatWorkOrigFn g_origFindOrAddGreatWork = nullptr;   // FindOrAddGreatWork 的原始实现

// ---------------------------------------------------------------------------
// 核心逻辑
// ---------------------------------------------------------------------------

// 前向声明（实现在下方"同一 greatWorkType 的多份支持"一节）
bool IndexInUse(void* buildings, int listIndex);
int  AppendDuplicateGreatWork(void* culture, unsigned int greatWorkType);

// 创建/查找巨作条目并设置归属玩家，返回 listIndex（失败返回 -1）。
//
// buildings 非空时：若拿到的 listIndex 已经躺在这个 Buildings 的某个槽里
// （说明是同一 greatWorkType 的第 2、3… 份），则强制新建一条唯一条目，
// 保证「一个 index 只在一个槽」，从而让引擎的 RemoveGreatWork 等函数行为正确。
// buildings 为空时退化为原始语义（纯查找）。
int EnsureGreatWork(void* culture, unsigned int greatWorkIndex, int playerId, void* buildings = nullptr) {
	if (!culture || !g_findOrAddGreatWork || !g_setGreatWorkPlayer) return -1;
	int listIndex = g_findOrAddGreatWork(culture, greatWorkIndex);
	if (listIndex < 0) return -1;

	if (buildings && IndexInUse(buildings, listIndex)) {
		const int dup = AppendDuplicateGreatWork(culture, greatWorkIndex);
		if (dup >= 0) {
			if (g_api) {
				g_api->Log("[GreatWorkAPI] duplicate great work: gw=%u reuse=%d -> new=%d",
					greatWorkIndex, listIndex, dup);
			}
			listIndex = dup;
		}
	}

	g_setGreatWorkPlayer(culture, listIndex, playerId);
	return listIndex;
}

// ---------------------------------------------------------------------------
// 同一 greatWorkType 的多份支持
//
// 【问题】FindOrAddGreatWork 按 greatWorkType 查找，同类型永远返回同一个 listIndex。
// 于是"多份同种巨作"= 同一个 index 出现在多个槽里。而引擎所有槽操作都假设
// 「一个 index 只在一个槽」：
//     RemoveGreatWork(index) 扫到第一个匹配就停，只清那一个。
// 结果就是移动巨作时清错槽（槽0 被清空、目标槽多出一个）。
//
// 【做法】不 hook、不改引擎。利用 FindOrAddGreatWork 自身的查找条件做"类型欺骗"：
//
//   1. 先正常调用，拿到 index i
//   2. 若 i 已被占用（已经躺在某个槽里）：
//        a. 临时把 greatWorks[i].greatWorkType 改成 -1
//        b. 再调一次 FindOrAddGreatWork(type) —— 查找必然未命中，
//           走引擎自己的追加分支，用 DoInsertValue 正确扩容并初始化新条目
//        c. 把 greatWorks[i].greatWorkType 改回原值
//        d. 返回新下标 j
//
// 这样完全复用了引擎的追加逻辑（内存管理、字段初始化、容量增长全部正确），
// 不需要手工构造 GreatWork、不需要调 DoInsertValue、不需要 hook 任何函数。
//
// 【安全性】a→c 之间是同步执行，中间没有其他游戏逻辑插入（单线程、无回调），
// 所以不会有代码观察到 type == -1 的瞬时状态。
//
// 【为什么不直接改 FindOrAddGreatWork】GrantRandomProduct / CreateHeroEpicGreatWork
// 依赖"连续两次调用返回同一 index"；无条件新建会让它们多插一条孤立条目，
// 并使后续所有巨作的 index 整体偏移（greatWorkIndex 是持久化的列表下标）。
// 因此只在"确实已被占用"时才新建。
// ---------------------------------------------------------------------------

// 判断某个 listIndex 是否已经出现在该 Buildings 的任意槽里。
bool IndexInUse(void* buildings, int listIndex) {
	if (!buildings || listIndex < 0) return false;
	char* b = static_cast<char*>(buildings);

	void* data = *reinterpret_cast<void**>(b + OFF_BLD_GW_DATA);
	const int count = *reinterpret_cast<int*>(b + OFF_BLD_GW_COUNT);
	if (!data || count <= 0 || count > kMaxSanityCount) return false;

	char* rec = static_cast<char*>(data);
	for (int i = 0; i < count; ++i, rec += BLD_GWINFO_SIZE) {
		void* slots = *reinterpret_cast<void**>(rec + OFF_GWINFO_SLOTS);
		const int num = *reinterpret_cast<int*>(rec + OFF_GWINFO_NUM);
		if (!slots || num <= 0 || num > kMaxSanityCount) continue;

		char* sp = static_cast<char*>(slots);
		for (int k = 0; k < num; ++k, sp += GWSLOT_SIZE) {
			if (*reinterpret_cast<int*>(sp + OFF_GWSLOT_INDEX) == listIndex) return true;
		}
	}
	return false;
}

// 取 Culture 的 greatWorks 列表基址与元素个数。
char* GetGreatWorkEntry(void* culture, int listIndex) {
	if (!culture || listIndex < 0) return nullptr;
	char* c = static_cast<char*>(culture);
	char* begin = *reinterpret_cast<char**>(c + OFF_CULTURE_GW_BEGIN);
	char* end   = *reinterpret_cast<char**>(c + OFF_CULTURE_GW_END);
	if (!begin || !end || end < begin) return nullptr;
	const ptrdiff_t count = (end - begin) / GW_ENTRY_SIZE;
	if (listIndex >= count) return nullptr;
	return begin + static_cast<ptrdiff_t>(GW_ENTRY_SIZE) * listIndex;
}

// 强制为指定 greatWorkType 追加一条新条目，返回新下标（失败 -1）。
//
// ============================================================================
// 【血泪教训：以下两种做法都会崩溃，不要再试】
//
//  1) 临时把既有条目的 greatWorkType 改成 -1，让查找未命中。
//     -> 崩溃于 CultureAi::DoOffTurn:
//          mov rax, [rdx+rdi*8]   ; rdi = 0xFFFFFFFF
//          mov rax, [rax+0B8h]    ; rax = 0 -> 访问 0xB8 -> ACCESS_VIOLATION
//        列表里存在 type == -1 的条目，AI 拿它查 Definition::GreatWork 得到
//        nullptr 后直接解引用 +0xB8。而且这个 -1 会持久化进存档，读档后继续崩。
//
//  2) 临时把 vector 的 end 指针前移一格，让查找看不到最后一条。
//     -> DoInsertValue 收到的 pos 正好等于前移后的 end，会走就地构造路径，
//        直接覆盖掉原最后一条，数据丢失。
// ============================================================================
//
// 【现方案】用一个列表里不存在的假类型触发追加，再把新条目的 type 改成目标类型。
//
//   1. 扫描列表，确认假类型未被占用（几乎必然成立）
//   2. 调 FindOrAddGreatWork(culture, fakeType) —— 查找必然未命中，
//      引擎走自己的 DoInsertValue 追加路径，正确扩容并初始化新条目
//   3. 重新取新条目地址（可能因扩容而移动），把 type 改成目标类型
//
// 优点：
//   · 不改任何既有条目
//   · 不碰 vector 的 begin / end / capacity
//   · 新条目的 player=-1 / turn / 名字全部由引擎按原生规则初始化
//   · fakeType 只在同步调用的极短窗口内存在，不会进入存档
//
// fakeType 选用 0x7FFFFFFF（int 上限），远离所有合法 GreatWorkTypes。
int AppendDuplicateGreatWork(void* culture, unsigned int greatWorkType) {
	if (!culture || !g_findOrAddGreatWork) return -1;
	if (greatWorkType == 0xFFFFFFFFu) return -1;   // -1 是哨兵值，不能当类型

	constexpr int kFakeType = 0x7FFFFFFF;

	// 1) 确认假类型未被占用
	char* c = static_cast<char*>(culture);
	char* begin = *reinterpret_cast<char**>(c + OFF_CULTURE_GW_BEGIN);
	char* end   = *reinterpret_cast<char**>(c + OFF_CULTURE_GW_END);
	if (!begin || !end || end < begin) return -1;

	for (char* p = begin; p < end; p += GW_ENTRY_SIZE) {
		if (*reinterpret_cast<int*>(p) == kFakeType) return -1;   // 极小概率，放弃
	}

	// 2) 用假类型触发追加（查找必然未命中）
	const int newIndex = g_findOrAddGreatWork(culture, static_cast<unsigned int>(kFakeType));
	if (newIndex < 0) return -1;

	// 3) 重新取条目地址（DoInsertValue 可能 realloc），改成目标类型
	char* entry = GetGreatWorkEntry(culture, newIndex);
	if (!entry) return -1;

	*reinterpret_cast<int*>(entry + OFF_GW_TYPE) = static_cast<int>(greatWorkType);
	return newIndex;
}

void* GetPlayerCities(int playerId) {
	if (!g_api || !g_api->GetPlayer) return nullptr;
	void* player = g_api->GetPlayer(playerId);
	if (!player) return nullptr;
	return *reinterpret_cast<void**>(static_cast<char*>(player) + OFF_PLAYER_CITIES);
}

// 遍历玩家所有城市，找第一个可放置的 Buildings 并放入（CE 语义）。
//
// 注意顺序：先找到能放的城市，再 EnsureGreatWork（要把 buildings 传进去，
// 这样同一类型的第 2、3… 份才会分配到唯一 index）。
bool AddGreatWorkAuto(void* culture, unsigned int greatWorkIndex, int playerId) {
	void* cities = GetPlayerCities(playerId);
	if (!cities) return false;

	// 第一轮：只做可行性探测（用临时 index 判断哪个城市放得下）
	const int probe = g_findOrAddGreatWork(culture, greatWorkIndex);
	if (probe < 0) {
		if (g_api) g_api->Log("[GreatWorkAPI] FindOrAddGreatWork failed (gw=%u)", greatWorkIndex);
		return false;
	}

	void** node = *reinterpret_cast<void***>(static_cast<char*>(cities) + OFF_CITIES_HEAD);
	while (node) {
		void* city = node[0];
		if (city) {
			void* buildings = static_cast<char*>(city) + OFF_CITY_BUILDINGS;
			if (g_canAddGreatWork(buildings, probe, nullptr)) {
				// 第二轮：针对这个城市分配唯一 index 并放入
				const int listIndex = EnsureGreatWork(culture, greatWorkIndex, playerId, buildings);
				if (listIndex < 0) return false;
				g_addGreatWork(buildings, listIndex);
				if (g_api) {
					g_api->Log("[GreatWorkAPI] AddGreatWork ok: player=%d gw=%u list=%d",
						playerId, greatWorkIndex, listIndex);
				}
				return true;
			}
		}
		node = reinterpret_cast<void**>(node[2]);
	}
	if (g_api) {
		g_api->Log("[GreatWorkAPI] AddGreatWork no free slot: player=%d gw=%u",
			playerId, greatWorkIndex);
	}
	return false;
}

// 指定城市放巨作（自动找该城可放建筑+槽位）。
bool AddGreatWorkToCityCore(void* culture, unsigned int greatWorkIndex, int playerId, int cityId) {
	void* cities = GetPlayerCities(playerId);
	if (!cities) return false;

	void* city = g_citiesFind(cities, cityId);
	if (!city) {
		if (g_api) g_api->Log("[GreatWorkAPI] city not found: player=%d city=%d", playerId, cityId);
		return false;
	}
	void* buildings = static_cast<char*>(city) + OFF_CITY_BUILDINGS;

	// 传 buildings：若该 index 已被本城某个槽占用，会自动新建唯一 index
	const int listIndex = EnsureGreatWork(culture, greatWorkIndex, playerId, buildings);
	if (listIndex < 0) return false;
	if (!g_canAddGreatWork(buildings, listIndex, nullptr)) {
		if (g_api) {
			g_api->Log("[GreatWorkAPI] city has no free slot: player=%d city=%d gw=%u",
				playerId, cityId, greatWorkIndex);
		}
		return false;
	}
	// 用 Buildings::AddGreatWork 而不是 AddGreatWorkToSlot(..., -1)。
	// 前者会逐槽校验 IsValidObjectForSlot（槽类别 vs 巨作类别），只放进合法空槽；
	// 后者只在该建筑记录里找第一个空槽直接写，不校验类别，会把著作塞进艺术槽之类。
	g_addGreatWork(buildings, listIndex);
	if (g_api) {
		g_api->Log("[GreatWorkAPI] AddGreatWorkToCity ok: player=%d city=%d gw=%u list=%d",
			playerId, cityId, greatWorkIndex, listIndex);
	}
	return true;
}

// ---------------------------------------------------------------------------
// Lua 封装（注册到全局 GreatWorkManager 表）
// ---------------------------------------------------------------------------

bool SymbolsReady() {
	return g_cultureEdit && g_findOrAddGreatWork && g_setGreatWorkPlayer &&
		g_canAddGreatWork && g_addGreatWork && g_addGreatWorkToSlot && g_citiesFind;
}

// 读第 index 个整数参数：类型不是 number 时返回 fallback（不抛 Lua 错误）。
constexpr int kLuaTNumber = 3;   // 标准 Lua 5.1 / HavokScript 的类型标签
int ReadIntArg(void* L, int index, int fallback) {
	if (!g_api) return fallback;
	if (g_api->LuaType(L, index) != kLuaTNumber) return fallback;
	return g_api->LuaToInteger(L, index);
}

// GreatWorkManager.AddGreatWork(playerID, greatWorkIndex) -> bool
int lAddGreatWork(void* L) {
	if (!g_api) return 0;
	if (!SymbolsReady()) {
		g_api->LuaPushBoolean(L, false);
		return 1;
	}
	const int playerId = ReadIntArg(L, 1, -1);
	const int gwIndex = ReadIntArg(L, 2, -1);
	if (playerId < 0 || gwIndex < 0) {
		g_api->LuaPushBoolean(L, false);
		return 1;
	}
	void* culture = g_cultureEdit();
	g_api->LuaPushBoolean(L, AddGreatWorkAuto(culture, static_cast<unsigned int>(gwIndex), playerId));
	return 1;
}

// GreatWorkManager.AddGreatWorkToCity(playerID, cityID, greatWorkIndex) -> bool
int lAddGreatWorkToCity(void* L) {
	if (!g_api) return 0;
	if (!SymbolsReady()) {
		g_api->LuaPushBoolean(L, false);
		return 1;
	}
	const int playerId = ReadIntArg(L, 1, -1);
	const int cityId = ReadIntArg(L, 2, -1);
	const int gwIndex = ReadIntArg(L, 3, -1);
	if (playerId < 0 || cityId < 0 || gwIndex < 0) {
		g_api->LuaPushBoolean(L, false);
		return 1;
	}
	void* culture = g_cultureEdit();
	g_api->LuaPushBoolean(L,
		AddGreatWorkToCityCore(culture, static_cast<unsigned int>(gwIndex), playerId, cityId));
	return 1;
}

// 每次脚本环境初始化都重挂一次：gameplay 与 UI 是两个独立的 lua_State。
void RegisterLuaGlobals(void* L) {
	if (!g_api || !L) return;
	g_api->LuaNewGlobalTable(L, "GreatWorkManager", 0, 2);
	g_api->LuaPushCFunctionField(L, -1, "AddGreatWork",
		reinterpret_cast<void*>(&lAddGreatWork), "lAddGreatWork");
	g_api->LuaPushCFunctionField(L, -1, "AddGreatWorkToCity",
		reinterpret_cast<void*>(&lAddGreatWorkToCity), "lAddGreatWorkToCity");
	g_api->LuaPop(L, 1);
}

} // namespace

// ---------------------------------------------------------------------------
// 入口 / 出口
// ---------------------------------------------------------------------------

void GreatWorkAPI_Init(HookAPI* api) {
	if (!api) return;
	g_api = api;

	g_cultureEdit = gc::ResolveVerified<CultureEditFn>(api, kHookImageGameCoreXP2,
		kTargetCultureEdit, kSigCultureEdit, sizeof(kSigCultureEdit), "GreatWorkAPI/Culture::Edit");
	g_findOrAddGreatWork = gc::ResolveVerified<FindOrAddGreatWorkFn>(api, kHookImageGameCoreXP2,
		kTargetFindOrAddGreatWork, kSigFindOrAdd, sizeof(kSigFindOrAdd), "GreatWorkAPI/FindOrAddGreatWork");
	g_setGreatWorkPlayer = gc::ResolveVerified<SetGreatWorkPlayerFn>(api, kHookImageGameCoreXP2,
		kTargetSetGreatWorkPlayer, kSigSetPlayer, sizeof(kSigSetPlayer), "GreatWorkAPI/SetGreatWorkPlayer");
	g_canAddGreatWork = gc::ResolveVerified<CanAddGreatWorkFn>(api, kHookImageGameCoreXP2,
		kTargetCanAddGreatWork, kSigCanAdd, sizeof(kSigCanAdd), "GreatWorkAPI/CanAddGreatWork");
	g_addGreatWork = gc::ResolveVerified<AddGreatWorkFn>(api, kHookImageGameCoreXP2,
		kTargetAddGreatWork, kSigAddGreatWork, sizeof(kSigAddGreatWork), "GreatWorkAPI/AddGreatWork");
	g_addGreatWorkToSlot = gc::ResolveVerified<AddGreatWorkToSlotFn>(api, kHookImageGameCoreXP2,
		kTargetAddGreatWorkToSlot, kSigAddToSlot, sizeof(kSigAddToSlot), "GreatWorkAPI/AddGreatWorkToSlot");
	g_citiesFind = gc::ResolveVerified<CitiesFindFn>(api, kHookImageGameCoreXP2,
		kTargetCitiesFind, kSigCitiesFind, sizeof(kSigCitiesFind), "GreatWorkAPI/Cities::Find");

	// 只挂 gameplay：这两个接口操作的是引擎 gameplay 域对象（Culture / Buildings），
	// UI 脚本环境用不到，也不该在那里暴露。
	api->RegisterGameCoreScriptForGamePlay(&RegisterLuaGlobals);

	api->Log("[GreatWorkAPI] symbols %s; GreatWorkManager.AddGreatWork/AddGreatWorkToCity registered",
		SymbolsReady() ? "ok" : "incomplete (lua api returns false)");
}

void GreatWorkAPI_Shutdown() {
	g_cultureEdit = nullptr;
	g_findOrAddGreatWork = nullptr;
	g_setGreatWorkPlayer = nullptr;
	g_canAddGreatWork = nullptr;
	g_addGreatWork = nullptr;
	g_addGreatWorkToSlot = nullptr;
	g_citiesFind = nullptr;
	g_api = nullptr;
}
