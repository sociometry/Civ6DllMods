// ModifierManagerAPI.cpp
// 修饰符挂接/摘除 —— 从 CivilizationVI_CommunityExtension-1.3.0-beta 的
// Unit.cpp（lAttachModifierByID / lDetachModifierByID）迁移到 Civ6DllLoader 插件框架。
//
// 对外接口：
//   unit:AttachModifierByID(modifierID)                 -> number   （实例方法，注入 IUnit）
//   ModifierManager.DetachModifierByID(owner, id[, n])  -> boolean  （owner 可为 unit/city/player/district）
//   ModifierManager.GetCount(owner, id)                 -> number
//
// Detach 不依赖本插件自己的台账，而是直接问引擎：
//   MakeObject(owner)  -> shared_ptr<IModifierObject>            （四种 owner 各一份重载）
//   GetModifiersOwnedBy(system, outVec, ownerSp)                  （IModifierObject::vftable+0x60
//                                                                 返回自有索引，再逐个查 map）
//   逐个比 ID -> ClearModifier(system, index) -> IModifier::Deactivate
// 这样引擎自己挂的层、别的插件挂的层、跨存档的层都能摘，与谁挂的无关。
//
// 只调 ClearModifier 不撤销属性加成（引擎每条 Remove 路径都是先 Deactivate 再
// ClearModifier），曾导致 maxMoves 摘除后仍停在 5。
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
// 只有修饰名、不带 AOB 兜底的目标，原因都是"前缀在当前映像里不唯一"（实测命中数）：
//   ScopedInstance<*,*>::GetInstance 的四个实例   731 处（三者前导字节完全相同）
//   IPlayer::GetPlayerInstance                    109 处
//   std::string::assign                            82 处
//   MakeObject(Unit/City/Player)                    3 处（正好是这三个重载）
// 命中多处时 Loader 只取第一个，兜底等于随机跳转，所以宁可放弃也不乱指。
// 它们的修饰名本身唯一，符号名解析 + 前导字节校验已足够。
//
// 前缀唯一（1 处）的目标才带 AOB 兜底，见下面 kTargetGetModifiersOwnedBy 等。
constexpr HookInfo kTargetUnitGetInstance = HookName(
	"?GetInstance@?$ScopedInstance@VIUnit@Lua@GameCore@@VInstance@Unit@3@@Lua@GameCore@@SAPEAVInstance@Unit@3@PEAUlua_State@@H_N@Z");

constexpr HookInfo kTargetStringAssign = HookName(
	"?assign@?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QEAAAEAV12@PEBD_K@Z");

// MakeObject 按 owner 类型重载；整份映像里只有这四个（Plot/Building/Team/GreatWork 等都没有）。
constexpr HookInfo kTargetMakeObjectUnit = HookName(
	"?MakeObject@GameEffects@@YA?AV?$shared_ptr@VIModifierObject@GameEffects@@@std@@PEBVInstance@Unit@GameCore@@@Z");
constexpr HookInfo kTargetMakeObjectCity = HookName(
	"?MakeObject@GameEffects@@YA?AV?$shared_ptr@VIModifierObject@GameEffects@@@std@@PEBVInstance@City@GameCore@@@Z");
constexpr HookInfo kTargetMakeObjectPlayer = HookName(
	"?MakeObject@GameEffects@@YA?AV?$shared_ptr@VIModifierObject@GameEffects@@@std@@PEBVInstance@Player@GameCore@@@Z");
// District 这个重载的前缀在映像里唯一（1 处），所以它是四个里唯一带 AOB 兜底的；
// 另外三个（Unit/City/Player）头部逐字节相同、命中 3 处，兜底会随机指到兄弟重载，
// 因此只写修饰名，靠 kSigMakeObject 做粗校验。
constexpr HookInfo kTargetMakeObjectDistrict = HookName(
	"?MakeObject@GameEffects@@YA?AV?$shared_ptr@VIModifierObject@GameEffects@@@std@@PEBVInstance@District@GameCore@@@Z",
	"40 53 48 83 EC 20 33 C0 48 89 54 24 40 48 8D 54");

// City / District / Player 的 Lua 取实例入口（Unit 的已在上面）。
// 注意 Player 拿到的是 Player::Instance*，与 MakeObject(Player) 对得上；
// 而 ScopedVirtualInstance<IPlayer,PlayerReference> 只返回 PlayerReference（ID 引用），不能用。
constexpr HookInfo kTargetCityGetInstance = HookName(
	"?GetInstance@?$ScopedVirtualInstance@VICity@Lua@GameCore@@VInstance@City@3@@Lua@GameCore@@SAPEAVInstance@City@3@PEAUlua_State@@H_N@Z");

constexpr HookInfo kTargetDistrictGetInstance = HookName(
	"?GetInstance@?$ScopedInstance@VIDistrict@Lua@GameCore@@VInstance@District@3@@Lua@GameCore@@SAPEAVInstance@District@3@PEAUlua_State@@H_N@Z");

constexpr HookInfo kTargetPlayerGetInstance = HookName(
	"?GetPlayerInstance@IPlayer@Lua@GameCore@@SAPEBVInstance@Player@3@PEAUlua_State@@H@Z");

// 按 owner+ID 查询引擎实际持有的层（Detach/GetCount 用，不依赖本插件台账）：
//   GetModifiersOwnedBy(system, outVec, ownerSp)  内部取 IModifierObject::vftable+0x60
//     返回的自有索引表，逐个查 system+96 的 map，命中的 shared_ptr<IModifier> 塞进 outVec
//   GetModifierOwnedByCount(system, idString, ownerSp) -> int
// 这两条是 Detach 的核心，特征码实测各 1 处命中，因此带 AOB 兜底。
constexpr HookInfo kTargetGetModifiersOwnedBy = HookName(
	"?GetModifiersOwnedBy@ModifierSystem@GameEffects@@QEBA?AV?$vector@V?$shared_ptr@VIModifier@GameEffects@@@std@@U?$Allocator@V?$shared_ptr@VIModifier@GameEffects@@@std@@@Memory@GameEffects@@@std@@AEBV?$shared_ptr@VIModifierObject@GameEffects@@@4@@Z",
	"48 89 5C 24 18 55 41 56 41 57 48 83 EC 20 45 33 FF");

constexpr HookInfo kTargetGetModifierOwnedByCount = HookName(
	"?GetModifierOwnedByCount@ModifierSystem@GameEffects@@QEBAHAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@AEBV?$shared_ptr@VIModifierObject@GameEffects@@@4@@Z",
	"41 54 41 56 41 57 48 83 EC 20 45 33 F6 4D 8B E0");

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

// 前导字节哨兵。注意这些是"粗校验"：只确认目标还是同一族函数，不区分族内成员。
//   kSigScopedInstance   Unit/City/District 三个 GetInstance 共用（模板实例化，头部逐字节相同）
//   kSigMakeObject       Unit/City/Player 三个 MakeObject 共用
// 族内成员靠修饰名区分，修饰名本身是唯一的；哨兵只拦"符号库指到无关函数"。
constexpr uint8_t kSigScopedInstance[8] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C };
constexpr uint8_t kSigPlayerGetInstance[8] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74 };
constexpr uint8_t kSigStringAssign[8]    = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74 };
constexpr uint8_t kSigMakeObject[8]      = { 0x48, 0x89, 0x6C, 0x24, 0x18, 0x57, 0x48, 0x81 };
constexpr uint8_t kSigMakeObjectDistrict[8] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x33, 0xC0 };
constexpr uint8_t kSigAttach[8]          = { 0x40, 0x53, 0x57, 0x41, 0x54, 0x41, 0x57, 0x48 };
constexpr uint8_t kSigClear[8]           = { 0x48, 0x83, 0xEC, 0x28, 0x4C, 0x8D, 0x49, 0x60 };
constexpr uint8_t kSigGetModifiersOwnedBy[8] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x41, 0x56 };
constexpr uint8_t kSigGetModifierOwnedByCount[8] = { 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83 };
// Game::Edit 是 mov rax, cs: ms_pkCurrentGame ; retn，中间 4 字节是 RIP 相对位移。
// 用带通配的特征码跨过 disp32，把校验延伸到 retn——否则只能比前 3 字节，
// 拦不住"符号库指到另一条 mov rax, [rip+xxx]"以外的形态。
constexpr auto kSigGameEdit = gc::ParseAob("48 8B 05 ?? ?? ?? ?? C3");
constexpr uint8_t kSigEditGameEffects[8] = { 0x48, 0x8B, 0x81, 0x60, 0x02, 0x00, 0x00, 0xC3 };
constexpr uint8_t kSigHandleGet[8]       = { 0x48, 0x8B, 0x01, 0xC3, 0xCC, 0xCC, 0xCC, 0xCC };

// DynamicModifier::ModifierIndex 字段偏移（CE 头文件逆向结论，当前版本未变）
constexpr size_t OFF_DYNAMIC_MODIFIER_INDEX = 0xE8;

// DynamicModifier 虚表（0x13FAC3E38）：
//   +0x08 GetInstanceId  -> *(int*)(this+0xE8)
//   +0x58 Activate       -> 置 this[256]=1
//   +0x60 Deactivate     -> this[304]==0 && this[256]!=0 时逐个 RemoveEffect
//   +0x68 IsActivated    -> this[256]
//   +0x70 WasActivated   -> this[280]
//   +0xA0 Disable        -> 只置 this[288]=1，不撤销任何效果（别拿它当 Deactivate）
// ClearModifier 只摘 map 项；引擎靠 ClearModifier 掉最后一份引用 -> 析构 -> Deactivate。
// 所以摘除必须显式调 Deactivate，否则效果会一直留在 owner 身上。
constexpr size_t VTBL_ACTIVATE = 0x58;
constexpr size_t VTBL_DEACTIVATE = 0x60;

// 撤销修饰符的属性加成。参数是 IModifier*。
void DeactivateModifier(void* modifier) {
	if (!modifier) return;
	void** vft = *reinterpret_cast<void***>(modifier);
	reinterpret_cast<void(__fastcall*)(void*)>(
		*reinterpret_cast<void**>(reinterpret_cast<char*>(vft) + VTBL_DEACTIVATE))(modifier);
}

// 摘项失败时的回滚：把加成重新挂回去。
void ReactivateModifier(void* modifier) {
	if (!modifier) return;
	void** vft = *reinterpret_cast<void***>(modifier);
	reinterpret_cast<void(__fastcall*)(void*)>(
		*reinterpret_cast<void**>(reinterpret_cast<char*>(vft) + VTBL_ACTIVATE))(modifier);
}

// shared_ptr 控制块（MSVC）：+8 强引用，+12 弱引用。台账靠它保活到 Deactivate 之后。
void SharedPtrRelease(void* control) {
	if (!control) return;
	if (_InterlockedExchangeAdd(
		reinterpret_cast<volatile long*>(static_cast<char*>(control) + 8), -1) != 1) {
		return;
	}
	void** vft = *reinterpret_cast<void***>(control);
	reinterpret_cast<void(__fastcall*)(void*)>(vft[0])(control);
	if (_InterlockedExchangeAdd(
		reinterpret_cast<volatile long*>(static_cast<char*>(control) + 12), -1) == 1) {
		reinterpret_cast<void(__fastcall*)(void*)>(vft[1])(control);
	}
}

HookAPI* g_api = nullptr;

typedef void* (__fastcall* GetInstanceFn)(void* L, int index, bool required);
typedef void* (__fastcall* StringAssignFn)(void* outStdString, const char* src, size_t len);
typedef void  (__fastcall* MakeObjectFn)(void* outShared16, const void* instance);
typedef void* (__fastcall* AttachModifierFn)(void* system, void* outResult24, void* ownerShared16,
	void* idStdString, bool activate);
typedef bool  (__fastcall* ClearModifierFn)(void* system, int mapIndex);
typedef void* (__cdecl*    GameEditFn)();
typedef void* (__fastcall* EditGameEffectsFn)(void* gameInstance);
typedef void* (__fastcall* HandleGetFn)(void* effectsHandle);
typedef void  (__fastcall* GetModifiersOwnedByFn)(void* system, void* outVector16,
	const void* ownerShared16);
typedef int   (__fastcall* GetModifierOwnedByCountFn)(void* system, const void* idStdString,
	const void* ownerShared16);

StringAssignFn      g_stringAssign = nullptr;
AttachModifierFn    g_attachModifier = nullptr;
ClearModifierFn     g_clearModifier = nullptr;
GameEditFn          g_gameEdit = nullptr;
EditGameEffectsFn   g_editGameEffects = nullptr;
HandleGetFn         g_handleGet = nullptr;
GetModifiersOwnedByFn     g_getModifiersOwnedBy = nullptr;
GetModifierOwnedByCountFn g_getModifierOwnedByCount = nullptr;

// 四种 owner：各自的 Lua 取实例入口 + MakeObject 重载
struct OwnerKind {
	const char* typeName;   // CheckScopedInstanceType 用的 TypeName
	GetInstanceFn getInstance;
	MakeObjectFn  makeObject;
};

OwnerKind g_ownerUnit     = { "Unit",     nullptr, nullptr };
OwnerKind g_ownerCity     = { "City",     nullptr, nullptr };
OwnerKind g_ownerPlayer   = { "Player",   nullptr, nullptr };
OwnerKind g_ownerDistrict = { "District", nullptr, nullptr };

// AttachModifier 的返回结构（24 字节；status 在 +0，shared_ptr 在 +8/+16）
struct AttachResult {
	int status;
	uint32_t pad;
	void* object;   // 创建出的 IModifier（DynamicModifier）
	void* control;  // shared_ptr 控制块
};
static_assert(sizeof(AttachResult) == 24, "AttachResult must be 24 bytes");

bool SymbolsReady() {
	return g_stringAssign && g_attachModifier && g_clearModifier && g_gameEdit &&
		g_editGameEffects && g_handleGet && g_getModifiersOwnedBy;
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
	if (!system || !g_ownerUnit.makeObject) {
		if (g_api) g_api->Log("%s", "[ModifierManager] modifier system unavailable");
		return -1;
	}

	// std::string：MSVC 布局 32 字节（16 字节 SSO 缓冲 + size + capacity）
	alignas(8) unsigned __int64 idString[4] = { 0, 0, 0, 0 };
	g_stringAssign(idString, modifierId, idLen);

	alignas(16) unsigned __int64 owner[2] = { 0, 0 };   // shared_ptr<IModifierObject>
	g_ownerUnit.makeObject(owner, unitInstance);

	AttachResult result{};
	g_attachModifier(system, &result, owner, idString, /*activate*/ true);

	int index = -1;
	if (result.status == 0 && result.object) {
		index = *reinterpret_cast<const int*>(
			static_cast<const char*>(result.object) + OFF_DYNAMIC_MODIFIER_INDEX);
		// AttachModifierResult 里带着一份 shared_ptr 引用，这里立即释放——引擎的
		// 生命周期约定是「map 里那份引用为唯一持有者，归零时析构并收尾」。
		// 长期攥着它会让对象永不析构，效果也就永远撤不掉。
		SharedPtrRelease(result.control);
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

// ---------------------------------------------------------------------------
// 按 owner + ID 直接问引擎（不依赖本插件台账）
// ---------------------------------------------------------------------------
// ModifierSystem 内部 map：this+0x60 是 _Myhead，节点布局
//   +0x00 _Left  +0x08 _Parent  +0x10 _Right  +0x19 _Isnil
//   +0x20 key(int)  +0x28 _Ptr(IModifier*)  +0x30 _Rep(控制块)
// 与 DestroyDynamicModifier / ClearModifier 里的查找循环完全一致。
constexpr size_t OFF_SYSTEM_MAP = 0x60;
constexpr size_t OFF_NODE_LEFT = 0x00;
constexpr size_t OFF_NODE_RIGHT = 0x10;
constexpr size_t OFF_NODE_ISNIL = 0x19;
constexpr size_t OFF_NODE_KEY = 0x20;
constexpr size_t OFF_NODE_PTR = 0x28;
constexpr size_t OFF_NODE_REP = 0x30;

// IModifier 虚表槽：+0x18 GetDefinition() -> shared_ptr<IModifierDefinition>*
// IModifierDefinition 虚表槽：+0x08 GetId() -> const std::string*
constexpr size_t VTBL_GET_DEFINITION = 0x18;
constexpr size_t VTBL_GET_ID = 0x08;

// IModifierObject 虚表槽：+0x60 返回该 owner 自有的修饰符索引 vector<int>{begin,end,cap}
constexpr size_t VTBL_OWNED_INDICES = 0x60;

// 一次查询的结果
struct OwnedModifier {
	int index;
	void* modifier;
	void* control;
};

// 从 map 里按索引取一层，找到时对控制块加 1（调用方负责 Release）。
bool LookupByIndex(void* system, int index, OwnedModifier* out) {
	char* head = *reinterpret_cast<char**>(static_cast<char*>(system) + OFF_SYSTEM_MAP);
	if (!head) return false;
	char* found = *reinterpret_cast<char**>(head + OFF_NODE_RIGHT);   // _Myhead->_Right
	char* candidate = head;
	while (found && !*reinterpret_cast<uint8_t*>(found + OFF_NODE_ISNIL)) {
		const int key = *reinterpret_cast<const int*>(found + OFF_NODE_KEY);
		if (key >= index) {
			candidate = found;
			found = *reinterpret_cast<char**>(found + OFF_NODE_LEFT);
		} else {
			found = *reinterpret_cast<char**>(found + OFF_NODE_RIGHT);
		}
	}
	if (candidate == head || *reinterpret_cast<const int*>(candidate + OFF_NODE_KEY) != index) {
		return false;
	}
	void* modifier = *reinterpret_cast<void**>(candidate + OFF_NODE_PTR);
	if (!modifier) return false;
	void* control = *reinterpret_cast<void**>(candidate + OFF_NODE_REP);
	if (control) {
		_InterlockedIncrement(reinterpret_cast<volatile long*>(static_cast<char*>(control) + 8));
	}
	out->index = index;
	out->modifier = modifier;
	out->control = control;
	return true;
}

// 取 IModifier 的 ModifierId（std::string*，MSVC 布局）。
const std::string* ModifierIdOf(void* modifier) {
	void** vft = *reinterpret_cast<void***>(modifier);
	void* defShared = reinterpret_cast<void*(__fastcall*)(void*)>(
		*reinterpret_cast<void**>(reinterpret_cast<char*>(vft) + VTBL_GET_DEFINITION))(modifier);
	if (!defShared) return nullptr;
	void* def = *reinterpret_cast<void**>(defShared);
	if (!def) return nullptr;
	void** defVft = *reinterpret_cast<void***>(def);
	typedef const std::string* (__fastcall* GetIdFn)(void*);
	GetIdFn fn = reinterpret_cast<GetIdFn>(
		*reinterpret_cast<void**>(reinterpret_cast<char*>(defVft) + VTBL_GET_ID));
	return fn(def);
}

// 枚举某 owner 当前持有的、ID 与给定字符串相同的所有层（按 map 索引升序）。
std::vector<OwnedModifier> FindOwnedBy(void* system, const void* ownerShared16,
	const char* modifierId, size_t idLen) {
	std::vector<OwnedModifier> result;
	if (!g_getModifiersOwnedBy || !system || !ownerShared16) return result;

	// vector<shared_ptr<IModifier>>：GameEffects::Memory::Allocator 就是三个指针
	alignas(8) unsigned __int64 owned[3] = { 0, 0, 0 };
	g_getModifiersOwnedBy(system, owned, ownerShared16);

	auto* begin = reinterpret_cast<char*>(owned[0]);
	auto* end = reinterpret_cast<char*>(owned[1]);
	if (begin && end > begin) {
		const size_t count = static_cast<size_t>(end - begin) / 16;
		for (size_t i = 0; i < count; ++i) {
			void* modifier = *reinterpret_cast<void**>(begin + i * 16);
			if (!modifier) continue;
			const std::string* id = ModifierIdOf(modifier);
			if (!id || id->size() != idLen || memcmp(id->data(), modifierId, idLen) != 0) {
				continue;
			}
			const int index = *reinterpret_cast<const int*>(
				static_cast<const char*>(modifier) + OFF_DYNAMIC_MODIFIER_INDEX);
			OwnedModifier item{ index, modifier, nullptr };
			// owned[] 里那份引用已经保证对象活着；再单独取控制块以便 Release。
			if (LookupByIndex(system, index, &item)) {
				result.push_back(item);
			} else {
				item.control = nullptr;
				result.push_back(item);
			}
		}
	}
	// 释放 owned 里的引用
	if (begin) {
		for (char* p = begin; p < end; p += 16) {
			void* control = *reinterpret_cast<void**>(p + 8);
			SharedPtrRelease(control);
		}
	}
	return result;
}

// 摘除 count 层（默认 1）。返回实际摘掉的层数。
int DetachCore(void* system, const void* ownerShared16, const char* modifierId,
	size_t idLen, int count) {
	if (!system) return 0;
	auto owned = FindOwnedBy(system, ownerShared16, modifierId, idLen);
	int removed = 0;
	for (const OwnedModifier& item : owned) {
		if (removed >= count) {
			SharedPtrRelease(item.control);
			continue;
		}
		// 先 Deactivate 撤销加成，再从 map 摘项：Deactivate 需要对象处于激活态
		// （IsActivated()==true），此时 map 里那份引用还在，对象一定活着。
		DeactivateModifier(item.modifier);
		if (!g_clearModifier(system, item.index)) {
			// Deactivate 已生效但摘项失败：把加成挂回去，保持一致。
			ReactivateModifier(item.modifier);
			SharedPtrRelease(item.control);
			continue;
		}
		SharedPtrRelease(item.control);
		++removed;
		if (g_api) {
			g_api->Log("[ModifierManager] detach ok: id=%s idx=%d",
				modifierId, item.index);
		}
	}
	return removed;
}

// 查询层数
int CountCore(void* system, const void* ownerShared16, const char* modifierId, size_t idLen) {
	if (!system) return 0;
	auto owned = FindOwnedBy(system, ownerShared16, modifierId, idLen);
	const int n = static_cast<int>(owned.size());
	for (const OwnedModifier& item : owned) {
		SharedPtrRelease(item.control);
	}
	return n;
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
	void* unit = g_ownerUnit.getInstance ? g_ownerUnit.getInstance(L, 1, true) : nullptr;
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

// 从 Lua 第 1 个参数按 TypeName 认出 owner 类型，并取实例 + MakeObject 重载。
//   owner 可以是 unit / city / player / district；其余类型返回 false。
bool ResolveOwner(void* L, const OwnerKind** kind, void** instance) {
	struct Candidate { const OwnerKind* kind; };
	const OwnerKind* all[] = { &g_ownerUnit, &g_ownerCity, &g_ownerPlayer, &g_ownerDistrict };
	for (const OwnerKind* k : all) {
		if (!k->getInstance) continue;
		void* inst = k->getInstance(L, 1, /*required*/ false);
		if (inst) {
			*kind = k;
			*instance = inst;
			return true;
		}
	}
	return false;
}

// 把 owner 实例包成 shared_ptr<IModifierObject>（16 字节），调用方负责 Release。
bool MakeOwnerShared(const OwnerKind* kind, void* instance, void* outShared16) {
	if (!kind || !kind->makeObject || !instance) return false;
	kind->makeObject(outShared16, instance);
	return *reinterpret_cast<void**>(outShared16) != nullptr;
}

// 读第 3 个参数（层数）。缺省 1；<=0 视为 1。
int ReadCountArg(void* L, int argIndex) {
	if (g_api->LuaGetTop(L) < argIndex) return 1;
	if (g_api->LuaType(L, argIndex) != kLuaTNumber) return 1;
	const int n = static_cast<int>(g_api->LuaToNumber(L, argIndex));
	return n < 1 ? 1 : n;
}

// ModifierManager.DetachModifierByID(owner, modifierID[, count]) -> number
//   返回实际摘掉的层数（0 表示没找到）。
int lDetachModifierByID(void* L) {
	if (!g_api) return 0;
	if (!SymbolsReady()) {
		g_api->LuaPushInteger(L, 0);
		return 1;
	}
	const OwnerKind* kind = nullptr;
	void* instance = nullptr;
	if (!ResolveOwner(L, &kind, &instance)) {
		g_api->LuaPushInteger(L, 0);
		return 1;
	}
	unsigned __int64 len = 0;
	const char* modifierId = g_api->LuaCheckLString(L, 2, &len);
	if (!modifierId || len == 0) {
		g_api->LuaPushInteger(L, 0);
		return 1;
	}
	void* system = ResolveModifierSystem();
	alignas(16) unsigned __int64 owner[2] = { 0, 0 };
	if (!system || !MakeOwnerShared(kind, instance, owner)) {
		g_api->LuaPushInteger(L, 0);
		return 1;
	}
	const int removed = DetachCore(system, owner, modifierId, static_cast<size_t>(len),
		ReadCountArg(L, 3));
	SharedPtrRelease(reinterpret_cast<void*>(owner[1]));
	g_api->LuaPushInteger(L, removed);
	return 1;
}

// ModifierManager.GetCount(owner, modifierID) -> number
int lGetCount(void* L) {
	if (!g_api) return 0;
	if (!SymbolsReady()) {
		g_api->LuaPushInteger(L, 0);
		return 1;
	}
	const OwnerKind* kind = nullptr;
	void* instance = nullptr;
	if (!ResolveOwner(L, &kind, &instance)) {
		g_api->LuaPushInteger(L, 0);
		return 1;
	}
	unsigned __int64 len = 0;
	const char* modifierId = g_api->LuaCheckLString(L, 2, &len);
	if (!modifierId || len == 0) {
		g_api->LuaPushInteger(L, 0);
		return 1;
	}
	void* system = ResolveModifierSystem();
	alignas(16) unsigned __int64 owner[2] = { 0, 0 };
	if (!system || !MakeOwnerShared(kind, instance, owner)) {
		g_api->LuaPushInteger(L, 0);
		return 1;
	}
	const int n = CountCore(system, owner, modifierId, static_cast<size_t>(len));
	SharedPtrRelease(reinterpret_cast<void*>(owner[1]));
	g_api->LuaPushInteger(L, n);
	return 1;
}

void RegisterLuaGlobals(void* L) {
	if (!g_api || !L) return;
	// owner 类型识别靠 TypeName 字符串，与引擎 CheckScopedInstanceType 的判定一致。
	g_api->LuaNewGlobalTable(L, "ModifierManager", 0, 1);
	g_api->LuaPushCFunctionField(L, -1, "DetachModifierByID",
		reinterpret_cast<void*>(&lDetachModifierByID), "lDetachModifierByID");
	g_api->LuaPushCFunctionField(L, -1, "GetCount",
		reinterpret_cast<void*>(&lGetCount), "lGetCount");
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

	g_stringAssign = gc::ResolveVerified<StringAssignFn>(api, kHookImageGameCoreXP2,
		kTargetStringAssign, kSigStringAssign, sizeof(kSigStringAssign),
		"ModifierManager/string::assign");

	// 四种 owner 的 Lua 取实例入口
	g_ownerUnit.getInstance = gc::ResolveVerified<GetInstanceFn>(api, kHookImageGameCoreXP2,
		kTargetUnitGetInstance, kSigScopedInstance, sizeof(kSigScopedInstance),
		"ModifierManager/Unit::GetInstance");
	g_ownerCity.getInstance = gc::ResolveVerified<GetInstanceFn>(api, kHookImageGameCoreXP2,
		kTargetCityGetInstance, kSigScopedInstance, sizeof(kSigScopedInstance),
		"ModifierManager/City::GetInstance");
	g_ownerDistrict.getInstance = gc::ResolveVerified<GetInstanceFn>(api, kHookImageGameCoreXP2,
		kTargetDistrictGetInstance, kSigScopedInstance, sizeof(kSigScopedInstance),
		"ModifierManager/District::GetInstance");
	g_ownerPlayer.getInstance = gc::ResolveVerified<GetInstanceFn>(api, kHookImageGameCoreXP2,
		kTargetPlayerGetInstance, kSigPlayerGetInstance, sizeof(kSigPlayerGetInstance),
		"ModifierManager/Player::GetPlayerInstance");

	// 四种 owner 的 MakeObject 重载（Detach/GetCount 要用它把实例包成 IModifierObject）
	g_ownerUnit.makeObject = gc::ResolveVerified<MakeObjectFn>(api, kHookImageGameCoreXP2,
		kTargetMakeObjectUnit, kSigMakeObject, sizeof(kSigMakeObject),
		"ModifierManager/MakeObject_Unit");
	g_ownerCity.makeObject = gc::ResolveVerified<MakeObjectFn>(api, kHookImageGameCoreXP2,
		kTargetMakeObjectCity, kSigMakeObject, sizeof(kSigMakeObject),
		"ModifierManager/MakeObject_City");
	g_ownerPlayer.makeObject = gc::ResolveVerified<MakeObjectFn>(api, kHookImageGameCoreXP2,
		kTargetMakeObjectPlayer, kSigMakeObject, sizeof(kSigMakeObject),
		"ModifierManager/MakeObject_Player");
	g_ownerDistrict.makeObject = gc::ResolveVerified<MakeObjectFn>(api, kHookImageGameCoreXP2,
		kTargetMakeObjectDistrict, kSigMakeObjectDistrict, sizeof(kSigMakeObjectDistrict),
		"ModifierManager/MakeObject_District");

	g_getModifiersOwnedBy = gc::ResolveVerified<GetModifiersOwnedByFn>(api, kHookImageGameCoreXP2,
		kTargetGetModifiersOwnedBy, kSigGetModifiersOwnedBy, sizeof(kSigGetModifiersOwnedBy),
		"ModifierManager/GetModifiersOwnedBy");
	g_getModifierOwnedByCount = gc::ResolveVerified<GetModifierOwnedByCountFn>(api,
		kHookImageGameCoreXP2, kTargetGetModifierOwnedByCount, kSigGetModifierOwnedByCount,
		sizeof(kSigGetModifierOwnedByCount), "ModifierManager/GetModifierOwnedByCount");
	g_attachModifier = gc::ResolveVerified<AttachModifierFn>(api, kHookImageGameCoreXP2,
		kTargetAttachModifier, kSigAttach, sizeof(kSigAttach),
		"ModifierManager/AttachModifier");
	g_clearModifier = gc::ResolveVerified<ClearModifierFn>(api, kHookImageGameCoreXP2,
		kTargetClearModifier, kSigClear, sizeof(kSigClear),
		"ModifierManager/ClearModifier");
	g_gameEdit = gc::ResolveVerifiedAob<GameEditFn>(api, kHookImageGameCoreXP2,
		kTargetGameEdit, "ModifierManager/Game::Edit", kSigGameEdit);
	g_editGameEffects = gc::ResolveVerified<EditGameEffectsFn>(api, kHookImageGameCoreXP2,
		kTargetEditGameEffects, kSigEditGameEffects, sizeof(kSigEditGameEffects),
		"ModifierManager/EditGameEffects");
	g_handleGet = gc::ResolveVerified<HandleGetFn>(api, kHookImageGameCoreXP2,
		kTargetHandleGet, kSigHandleGet, sizeof(kSigHandleGet), "ModifierManager/Handle::get");

	api->RegisterGameCoreScriptForGamePlay(&RegisterLuaGlobals);


	if (SymbolsReady()) {
		InjectUnitMethod();
		api->Log("%s", "[ModifierManager] symbols resolved; unit:AttachModifierByID available");
	} else {
		api->Log("%s", "[ModifierManager] symbols incomplete - lua api fails gracefully");
	}
}

void ModifierManagerAPI_Shutdown() {
	g_stringAssign = nullptr;
	g_attachModifier = nullptr;
	g_clearModifier = nullptr;
	g_gameEdit = nullptr;
	g_editGameEffects = nullptr;
	g_handleGet = nullptr;
	g_getModifiersOwnedBy = nullptr;
	g_getModifierOwnedByCount = nullptr;
	for (OwnerKind* k : { &g_ownerUnit, &g_ownerCity, &g_ownerPlayer, &g_ownerDistrict }) {
		k->getInstance = nullptr;
		k->makeObject = nullptr;
	}
	g_api = nullptr;
}
