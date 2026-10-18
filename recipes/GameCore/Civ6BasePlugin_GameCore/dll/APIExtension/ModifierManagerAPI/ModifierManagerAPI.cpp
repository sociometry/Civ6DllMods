// ModifierManagerAPI.cpp
// 单位修饰符挂接/摘除 —— 从 CivilizationVI_CommunityExtension-1.3.0-beta 的
// Unit.cpp（lAttachModifierByID / lDetachModifierByID）迁移到 Civ6DllLoader 插件框架。
//
// 相对上一版的改动（兼容性修复）：
//   1) 删除 RegisterGameCoreReady / RegisterGameCoreTeardown（OnLoad 时 GameCore 已就绪）；
//   2) 取址改为符号名 + AOB 兜底 + 前导字节校验（含 std::string::assign 的修饰名）；
//   3) 管理器链不再读硬编码的 ms_pkCurrentGame 全局地址，改为调用
//      GameCore::Game::Instance::Edit()（引擎自己的访问器，内部就是读那个全局）；
//   4) Lua 参数改用 LuaType 判断 + LuaCheckLString，非法参数返回 -1/false 而不抛错。
//
// 已在当前 Steam 版 IDB 逐一验证的符号（修饰名优先，跨版本由 PdataMatch 矫正）：
//   GameCore::Lua::ScopedInstance<Lua::IUnit,Unit::Instance>::GetInstance
//        (L, index, bool) -> Unit::Instance*（Lua 栈 -> 单位实例）
//   std::string::assign(const char*, size_t)
//        (out std::string, src, len)：构造 ModifierDefinition 的 ID 字符串
//   GameEffects::MakeObject(Unit::Instance const*) -> shared_ptr<IModifierObject>
//        以单位实例为 owner 造出修饰符挂载对象（sret 返回 16 字节 shared_ptr）
//   GameEffects::ModifierSystem::AttachModifier_1
//        (system, out result[24B], ownerSp, idString, activate)
//   GameEffects::ModifierSystem::ClearModifier_1
//        (system, mapIndex) -> bool，按 int 键删除一条
//   GameCore::Game::Instance::Edit()            -> Game::Instance&
//   GameCore::Game::Instance::EditGameEffects() -> GameEffects::Instance*
//   GameCore::AI::Control::Handle::get()        -> ModifierSystem*（= *handle）
//
// Attach 语义复刻原生 IPlayer/CE Unit 版本：
//   owner = MakeObject_Unit(unitInstance)          -- 16B shared_ptr 栈缓冲
//   id    = std::string::assign(modifierID)        -- MSVC std::string
//   result{status, objPtr, ctrl}                   -- 24B
//   status==0 且 objPtr 有效 => 索引 = *(int*)(objPtr + 0xE8)（DynamicModifier::ModifierIndex）
//
// Detach 实现：Attach 成功时登记三元组 {unitInstance, modifierID, index}；Detach 线性
// 查找首个匹配记录 -> ClearModifier(system, index) 摘除一层 -> 无论成败都弹出该记录
// （失败即记录已失效），继续找下一层直到成功或穷尽。
// 覆盖范围：只跟踪经本接口挂上的层；引擎自发挂的不受控。
//
// 线程模型：OnLoad 与全部 Lua 回调都在游戏主线程，注册表无锁。

#include "pch.h"
#include "HookAPI.h"
#include "ModifierManagerAPI.h"
#include "../../Common/GameCoreResolve.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// 目标描述
// ---------------------------------------------------------------------------
// 下面三个目标的样板前缀在当前映像里都有多份同形命中（ScopedInstance::GetInstance 系列
// 有 50 处、std::string::assign 有 5 处、MakeObject<...> 系列有 3 处），因此**不带 AOB 兜底**，
// 只按修饰名解析 + 前导字节校验——它们的修饰名本身是唯一的。
constexpr HookInfo kTargetUnitGetInstance = HookName(
	"?GetInstance@?$ScopedInstance@VIUnit@Lua@GameCore@@VInstance@Unit@3@@Lua@GameCore@@SAPEAVInstance@Unit@3@PEAUlua_State@@H_N@Z");

constexpr HookInfo kTargetStringAssign = HookName(
	"?assign@?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QEAAAEAV12@PEBD_K@Z");

constexpr HookInfo kTargetMakeObjectUnit = HookName(
	"?MakeObject@GameEffects@@YA?AV?$shared_ptr@VIModifierObject@GameEffects@@@std@@PEBVInstance@Unit@GameCore@@@Z");

constexpr HookInfo kTargetAttachModifier = HookName(
	"?AttachModifier@ModifierSystem@GameEffects@@QEAA?AUAttachModifierResult@2@AEBV?$shared_ptr@VIModifierObject@GameEffects@@@std@@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@5@_N@Z",
	"40 53 57 41 54 41 57 48 83 EC 48 49 83 38 00 49 8B D9");

constexpr HookInfo kTargetClearModifier = HookName(
	"?ClearModifier@ModifierSystem@GameEffects@@QEAA_NH@Z",
	"48 83 EC 28 4C 8D 49 60 48 8B 49 60 4C 8B C1 48 8B 41 08");

constexpr HookInfo kTargetGameEdit = HookName(
	"GameCore::Game::Instance::Edit",
	"48 8B 05 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC 48 8D 05");

constexpr HookInfo kTargetEditGameEffects = HookName(
	"GameCore::Game::Instance::EditGameEffects",
	"48 8B 81 60 02 00 00 C3 CC CC CC CC CC CC CC CC");

constexpr HookInfo kTargetHandleGet = HookName(
	"GameCore::AI::Control::Handle::get",
	"48 8B 01 C3 CC CC CC CC CC CC CC CC CC CC CC CC 48 83 EC 28 48 8B C2");

// 前导字节哨兵
constexpr uint8_t kSigUnitGetInstance[8] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C };
constexpr uint8_t kSigStringAssign[8]    = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74 };
constexpr uint8_t kSigMakeObject[8]      = { 0x48, 0x89, 0x6C, 0x24, 0x18, 0x57, 0x48, 0x81 };
constexpr uint8_t kSigAttach[8]          = { 0x40, 0x53, 0x57, 0x41, 0x54, 0x41, 0x57, 0x48 };
constexpr uint8_t kSigClear[8]           = { 0x48, 0x83, 0xEC, 0x28, 0x4C, 0x8D, 0x49, 0x60 };
// 前导字节只能比到 disp32 之前：mov rax, cs:xxx 的后 4 字节是 RIP 相对位移，
// 随版本与排布变化，写死成 00 00 00 00 会让校验永远失败（曾导致整个模块被跳过）。
// 只校验 48 8B 05（mov rax, [rip+disp32]）这三字节稳定形态。
constexpr uint8_t kSigGameEditPrefix[3] = { 0x48, 0x8B, 0x05 };
constexpr uint8_t kSigEditGameEffects[8] = { 0x48, 0x8B, 0x81, 0x60, 0x02, 0x00, 0x00, 0xC3 };
constexpr uint8_t kSigHandleGet[8]       = { 0x48, 0x8B, 0x01, 0xC3, 0xCC, 0xCC, 0xCC, 0xCC };

// DynamicModifier::ModifierIndex 字段偏移（CE 头文件逆向结论，当前版本未变）
constexpr size_t OFF_DYNAMIC_MODIFIER_INDEX = 0xE8;

HookAPI* g_api = nullptr;

typedef void* (__cdecl*    UnitGetInstanceFn)(void* L, int index, bool required);
typedef void* (__fastcall* StringAssignFn)(void* outStdString, const char* src, size_t len);
typedef void  (__fastcall* MakeObjectUnitFn)(void* outShared16, void* unitInstance);
typedef void* (__fastcall* AttachModifierFn)(void* system, void* outResult24, void* ownerShared16,
	void* idStdString, bool activate);
typedef bool  (__fastcall* ClearModifierFn)(void* system, int mapIndex);
typedef void* (__cdecl*    GameEditFn)();
typedef void* (__fastcall* EditGameEffectsFn)(void* gameInstance);
typedef void* (__fastcall* HandleGetFn)(void* effectsHandle);

UnitGetInstanceFn   g_unitGetInstance = nullptr;
StringAssignFn      g_stringAssign = nullptr;
MakeObjectUnitFn    g_makeObjectUnit = nullptr;
AttachModifierFn    g_attachModifier = nullptr;
ClearModifierFn     g_clearModifier = nullptr;
GameEditFn          g_gameEdit = nullptr;
EditGameEffectsFn   g_editGameEffects = nullptr;
HandleGetFn         g_handleGet = nullptr;

// ---------------------------------------------------------------------------
// 本插件挂上的修饰符台账：{单位实例, ID, map 索引}
// ---------------------------------------------------------------------------
struct AttachedRecord {
	void* unitInstance;
	std::string modifierId;
	int index;
};
std::vector<AttachedRecord> g_attached;

// AttachModifier 的返回结构（24 字节；status 在 +0，shared_ptr 在 +8/+16）
struct AttachResult {
	int status;
	uint32_t pad;
	void* object;   // 创建出的 IModifier（DynamicModifier）
	void* control;  // shared_ptr 控制块
};
static_assert(sizeof(AttachResult) == 24, "AttachResult must be 24 bytes");

bool SymbolsReady() {
	return g_unitGetInstance && g_stringAssign && g_makeObjectUnit && g_attachModifier &&
		g_clearModifier && g_gameEdit && g_editGameEffects && g_handleGet;
}

// 原生绑定同款管理器链：Game::Instance -> GameEffects -> ModifierSystem
void* ResolveModifierSystem() {
	if (!g_gameEdit || !g_editGameEffects || !g_handleGet) return nullptr;
	void* game = g_gameEdit();
	if (!game) return nullptr;
	void* effects = g_editGameEffects(game);
	if (!effects) return nullptr;
	return g_handleGet(effects);
}

// 给单位挂一层 modifierID 并激活。成功时把（单位, ID, 索引）计入台账并返回索引，
// 失败返回 -1。
int AttachCore(void* unitInstance, const char* modifierId, size_t idLen) {
	void* system = ResolveModifierSystem();
	if (!system) {
		if (g_api) g_api->Log("%s", "[ModifierManager] modifier system unavailable");
		return -1;
	}

	// std::string：MSVC 布局 32 字节（16 字节 SSO 缓冲 + size + capacity）
	alignas(8) unsigned __int64 idString[4] = { 0, 0, 0, 0 };
	g_stringAssign(idString, modifierId, idLen);

	alignas(16) unsigned __int64 owner[2] = { 0, 0 };   // shared_ptr<IModifierObject>
	g_makeObjectUnit(owner, unitInstance);

	AttachResult result{};
	g_attachModifier(system, &result, owner, idString, /*activate*/ true);

	int index = -1;
	if (result.status == 0 && result.object) {
		index = *reinterpret_cast<const int*>(
			static_cast<const char*>(result.object) + OFF_DYNAMIC_MODIFIER_INDEX);
		g_attached.push_back({ unitInstance, std::string(modifierId, idLen), index });
		if (g_api) {
			g_api->Log("[ModifierManager] attach ok: unit=%p id=%s idx=%d",
				unitInstance, modifierId, index);
		}
	} else if (g_api) {
		g_api->Log("[ModifierManager] attach failed: unit=%p id=%s status=%d",
			unitInstance, modifierId, result.status);
	}
	return index;
}

// 摘除一层：找首个匹配记录 -> ClearModifier 该索引 -> 弹出记录；
// ClearModifier 失败说明该层已被游戏侧回收，弹掉失效记录后继续找下一层。
bool DetachCore(void* unitInstance, const char* modifierId, size_t idLen) {
	void* system = ResolveModifierSystem();
	if (!system) return false;

	for (size_t i = 0; i < g_attached.size(); ++i) {
		AttachedRecord& rec = g_attached[i];
		if (rec.unitInstance != unitInstance ||
			rec.modifierId.size() != idLen ||
			memcmp(rec.modifierId.data(), modifierId, idLen) != 0) {
			continue;
		}
		const bool removed = g_clearModifier(system, rec.index);
		const int index = rec.index;
		g_attached.erase(g_attached.begin() + static_cast<ptrdiff_t>(i));
		if (removed) {
			if (g_api) {
				g_api->Log("[ModifierManager] detach ok: unit=%p id=%s idx=%d",
					unitInstance, modifierId, index);
			}
			return true;
		}
		if (g_api) {
			g_api->Log("[ModifierManager] stale record dropped: unit=%p id=%s idx=%d",
				unitInstance, modifierId, index);
		}
		--i;   // erase 后原地续查（还有下一层同名修饰符的情况）
	}
	return false;
}

// ---------------------------------------------------------------------------
// Lua 封装
// ---------------------------------------------------------------------------
constexpr int kLuaTNumber = 3;

// unitInstance:AttachModifierByID(modifierID) -> number
int lAttachModifierByID(void* L) {
	if (!g_api) return 0;
	if (!SymbolsReady()) {
		g_api->LuaPushInteger(L, -1);
		return 1;
	}
	void* unit = g_unitGetInstance(L, 1, true);
	if (!unit) {
		g_api->LuaPushInteger(L, -1);
		return 1;
	}
	unsigned __int64 len = 0;
	const char* modifierId = g_api->LuaCheckLString(L, 2, &len);
	if (!modifierId || len == 0) {
		g_api->LuaPushInteger(L, -1);
		return 1;
	}
	g_api->LuaPushInteger(L, AttachCore(unit, modifierId, static_cast<size_t>(len)));
	return 1;
}

// ModifierManager.DetachModifierByID(unitInstance, modifierID) -> boolean
int lDetachModifierByID(void* L) {
	if (!g_api) return 0;
	if (!SymbolsReady()) {
		g_api->LuaPushBoolean(L, false);
		return 1;
	}
	void* unit = g_unitGetInstance(L, 1, true);
	if (!unit) {
		g_api->LuaPushBoolean(L, false);
		return 1;
	}
	unsigned __int64 len = 0;
	const char* modifierId = g_api->LuaCheckLString(L, 2, &len);
	if (!modifierId || len == 0) {
		g_api->LuaPushBoolean(L, false);
		return 1;
	}
	g_api->LuaPushBoolean(L, DetachCore(unit, modifierId, static_cast<size_t>(len)));
	return 1;
}

void RegisterLuaGlobals(void* L) {
	if (!g_api || !L) return;
	g_api->LuaNewGlobalTable(L, "ModifierManager", 0, 1);
	g_api->LuaPushCFunctionField(L, -1, "DetachModifierByID",
		reinterpret_cast<void*>(&lDetachModifierByID), "lDetachModifierByID");
	g_api->LuaPop(L, 1);
}

// 把 Attach 以实例方法注入进 IUnit 的方法表（RegisterPushMethodsTarget + PushLuaMethods）。
void InjectUnitMethod() {
	static LuaMethodInfo s_unitAttach = {
		&lAttachModifierByID, "lAttachModifierByID", "AttachModifierByID"
	};
	const int rc = g_api->RegisterPushMethodsTarget("GameCore::Lua::IUnit",
		kHookApiPushNoManager, "GameCore::Lua::IUnit::PushMethods");
	const HookApiPushMethodsResult pushed =
		g_api->PushLuaMethods("GameCore::Lua::IUnit", &s_unitAttach, 1);
	g_api->Log("[ModifierManager] unit-method inject: reg=%d pushed=%d (unit:AttachModifierByID(id))",
		rc, static_cast<int>(pushed));
}

} // namespace

// ---------------------------------------------------------------------------
// 入口 / 出口
// ---------------------------------------------------------------------------

void ModifierManagerAPI_Init(HookAPI* api) {
	if (!api) return;
	g_api = api;

	g_unitGetInstance = gc::ResolveVerified<UnitGetInstanceFn>(api, kHookImageGameCoreXP2,
		kTargetUnitGetInstance, kSigUnitGetInstance, sizeof(kSigUnitGetInstance),
		"ModifierManager/Unit::GetInstance");
	g_stringAssign = gc::ResolveVerified<StringAssignFn>(api, kHookImageGameCoreXP2,
		kTargetStringAssign, kSigStringAssign, sizeof(kSigStringAssign),
		"ModifierManager/string::assign");
	g_makeObjectUnit = gc::ResolveVerified<MakeObjectUnitFn>(api, kHookImageGameCoreXP2,
		kTargetMakeObjectUnit, kSigMakeObject, sizeof(kSigMakeObject),
		"ModifierManager/MakeObject_Unit");
	g_attachModifier = gc::ResolveVerified<AttachModifierFn>(api, kHookImageGameCoreXP2,
		kTargetAttachModifier, kSigAttach, sizeof(kSigAttach),
		"ModifierManager/AttachModifier");
	g_clearModifier = gc::ResolveVerified<ClearModifierFn>(api, kHookImageGameCoreXP2,
		kTargetClearModifier, kSigClear, sizeof(kSigClear),
		"ModifierManager/ClearModifier");
	g_gameEdit = gc::ResolveVerified<GameEditFn>(api, kHookImageGameCoreXP2,
		kTargetGameEdit, kSigGameEditPrefix, sizeof(kSigGameEditPrefix), "ModifierManager/Game::Edit");
	g_editGameEffects = gc::ResolveVerified<EditGameEffectsFn>(api, kHookImageGameCoreXP2,
		kTargetEditGameEffects, kSigEditGameEffects, sizeof(kSigEditGameEffects),
		"ModifierManager/EditGameEffects");
	g_handleGet = gc::ResolveVerified<HandleGetFn>(api, kHookImageGameCoreXP2,
		kTargetHandleGet, kSigHandleGet, sizeof(kSigHandleGet), "ModifierManager/Handle::get");

	g_attached.clear();

	api->RegisterGameCoreScriptForGamePlay(&RegisterLuaGlobals);
	api->RegisterGameCoreScriptForUI(&RegisterLuaGlobals);

	if (SymbolsReady()) {
		InjectUnitMethod();
		api->Log("%s", "[ModifierManager] symbols resolved; unit:AttachModifierByID available");
	} else {
		api->Log("%s", "[ModifierManager] symbols incomplete - lua api fails gracefully");
	}
}

void ModifierManagerAPI_Shutdown() {
	g_attached.clear();
	g_unitGetInstance = nullptr;
	g_stringAssign = nullptr;
	g_makeObjectUnit = nullptr;
	g_attachModifier = nullptr;
	g_clearModifier = nullptr;
	g_gameEdit = nullptr;
	g_editGameEffects = nullptr;
	g_handleGet = nullptr;
	g_api = nullptr;
}
