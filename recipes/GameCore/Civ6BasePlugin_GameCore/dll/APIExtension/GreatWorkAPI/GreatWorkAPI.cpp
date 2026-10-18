// GreatWorkAPI.cpp
// 巨作添加整合接口（APIExtension 分类）：两个开箱即用的 Lua 接口，Lua 侧无需手工组合。
//
// 相对上一版的改动（兼容性修复）：
//   1) EditPlayer -> GetPlayer（HookAPI.h 中的现名）；
//   2) 删除 RegisterGameCoreReady / RegisterGameCoreTeardown，改为 OnLoad 内直接解析；
//   3) 全部取址改为符号名（含修饰名）+ AOB 兜底 + 前导字节校验，不再依赖裸 RVA；
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

// 前导字节哨兵
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

HookAPI* g_api = nullptr;

typedef void* (__cdecl*    CultureEditFn)();
typedef int   (__fastcall* FindOrAddGreatWorkFn)(void* culture, unsigned int greatWorkIndex);
typedef void  (__fastcall* SetGreatWorkPlayerFn)(void* culture, int listIndex, int playerId);
typedef bool  (__fastcall* CanAddGreatWorkFn)(void* buildings, int listIndex, int* outBuildingType);
typedef void  (__fastcall* AddGreatWorkFn)(void* buildings, int listIndex);
typedef void  (__fastcall* AddGreatWorkToSlotFn)(void* buildings, int listIndex, int buildingType, int slotIndex);
typedef void* (__fastcall* CitiesFindFn)(void* cities, int cityId);

CultureEditFn           g_cultureEdit = nullptr;
FindOrAddGreatWorkFn    g_findOrAddGreatWork = nullptr;
SetGreatWorkPlayerFn    g_setGreatWorkPlayer = nullptr;
CanAddGreatWorkFn       g_canAddGreatWork = nullptr;
AddGreatWorkFn          g_addGreatWork = nullptr;
AddGreatWorkToSlotFn    g_addGreatWorkToSlot = nullptr;
CitiesFindFn            g_citiesFind = nullptr;

// ---------------------------------------------------------------------------
// 核心逻辑
// ---------------------------------------------------------------------------

// 创建/查找巨作条目并设置归属玩家，返回 listIndex（失败返回 -1）。
int EnsureGreatWork(void* culture, unsigned int greatWorkIndex, int playerId) {
	if (!culture || !g_findOrAddGreatWork || !g_setGreatWorkPlayer) return -1;
	const int listIndex = g_findOrAddGreatWork(culture, greatWorkIndex);
	if (listIndex < 0) return -1;
	g_setGreatWorkPlayer(culture, listIndex, playerId);
	return listIndex;
}

void* GetPlayerCities(int playerId) {
	if (!g_api || !g_api->GetPlayer) return nullptr;
	void* player = g_api->GetPlayer(playerId);
	if (!player) return nullptr;
	return *reinterpret_cast<void**>(static_cast<char*>(player) + OFF_PLAYER_CITIES);
}

// 遍历玩家所有城市，找第一个可放置的 Buildings 并放入（CE 语义）。
bool AddGreatWorkAuto(void* culture, unsigned int greatWorkIndex, int playerId) {
	const int listIndex = EnsureGreatWork(culture, greatWorkIndex, playerId);
	if (listIndex < 0) {
		if (g_api) g_api->Log("[GreatWorkAPI] FindOrAddGreatWork failed (gw=%u)", greatWorkIndex);
		return false;
	}

	void* cities = GetPlayerCities(playerId);
	if (!cities) return false;

	void** node = *reinterpret_cast<void***>(static_cast<char*>(cities) + OFF_CITIES_HEAD);
	while (node) {
		void* city = node[0];
		if (city) {
			void* buildings = static_cast<char*>(city) + OFF_CITY_BUILDINGS;
			int buildingType = -1;
			if (g_canAddGreatWork(buildings, listIndex, &buildingType)) {
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
	const int listIndex = EnsureGreatWork(culture, greatWorkIndex, playerId);
	if (listIndex < 0) return false;

	void* cities = GetPlayerCities(playerId);
	if (!cities) return false;

	void* city = g_citiesFind(cities, cityId);
	if (!city) {
		if (g_api) g_api->Log("[GreatWorkAPI] city not found: player=%d city=%d", playerId, cityId);
		return false;
	}
	void* buildings = static_cast<char*>(city) + OFF_CITY_BUILDINGS;
	int buildingType = -1;
	if (!g_canAddGreatWork(buildings, listIndex, &buildingType)) {
		if (g_api) {
			g_api->Log("[GreatWorkAPI] city has no free slot: player=%d city=%d gw=%u",
				playerId, cityId, greatWorkIndex);
		}
		return false;
	}
	// slotIndex = -1 表示自动找空槽（与引擎内部调用一致）
	g_addGreatWorkToSlot(buildings, listIndex, buildingType, -1);
	if (g_api) {
		g_api->Log("[GreatWorkAPI] AddGreatWorkToCity ok: player=%d city=%d gw=%u list=%d bld=%d",
			playerId, cityId, greatWorkIndex, listIndex, buildingType);
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

	api->RegisterGameCoreScriptForGamePlay(&RegisterLuaGlobals);
	api->RegisterGameCoreScriptForUI(&RegisterLuaGlobals);

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
