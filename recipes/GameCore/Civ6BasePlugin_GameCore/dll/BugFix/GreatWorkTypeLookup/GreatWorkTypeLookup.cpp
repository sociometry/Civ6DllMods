// GreatWorkTypeLookup.cpp
// 巨作「名哈希 -> 类型索引」解析修复。
//
// 槽位类型部分从 CivilizationVI_CommunityExtension-1.3.0-beta 的 Main.cpp
// （PatternTest + Hook_S）迁移；物件类型部分按同一缺陷模式扩展（CE 未覆盖）。
//
// 相对上一版的改动（兼容性修复）：
//   1) 删除 RegisterGameCoreReady / RegisterGameCoreTeardown：本工程是 GameCore 插件，
//      OnLoad 时 GameCore 已就绪，代际回调没有意义；退局由 Loader 按 owner 回滚。
//   2) 取址改为「符号名（含修饰名）为主 + AOB 兜底」，
//      不再依赖裸 RVA（见 Common/GameCoreResolve.h）。
//   3) 缓存表加容量上限，避免异常哈希把表撑爆。
//
// 反编译核实（当前 Steam 版 GameCore_XP2_FinalRelease.dll IDB）：
//
// [1] Definition::Utility::NameToGreatWorkSlotType —— 硬编码 switch：
//     MUSIC 0x2F6C7BC9 / RELIC 0x8ABFC848 / PALACE 0xC8609506 / HERO 0xD8878949 /
//     WRITING 0x00B22BE8 / PRODUCT 0x3E60D2DE / ARTIFACT 0x6868A539 /
//     ART 0x699389CB / CATHEDRAL 0x6DB6854C ；default -> -1
//     首条指令 cmp ecx,0x2F6C7BC9（= 81 F9 C9 7B 6C 2F），AOB 唯一命中。
//
// [2] Types::GetIndex(GreatWorkSlotTypes) —— [1] 的数据驱动解析器：
//     入参 < 定义数 -> 索引透传；否则在定义集合内按哈希 Find，命中返回索引，否则 -1。
//     只有修饰名唯一（demangled 名 Types::GetIndex_26 带序号，跨版本可能变），
//     因此这里用 **MSVC 修饰名** 作为主路径。
//
// [3] Definition::Utility::NameToGreatWorkObjectType —— 同 [1] 形态：
//     SCULPTURE 0x05AFB7A8 / PRODUCT 0x8869D47F / RELIGIOUS 0xACA72322 /
//     MUSIC 0xB487A51F / WRITING 0xB6BB2D49 / ARTIFACT 0xC90F3F41 /
//     RELIC 0x1154169E / PORTRAIT 0x14AA6B96 / LANDSCAPE 0x1E0E0BF4 /
//     HERO 0x579BEF89 ；default -> -1
//
// [4] Types::GetIndex(GreatWorkObjectTypes) —— [3] 的数据驱动解析器，形态同 [2]。
//
// 兼容性影响面（四个调用面均受益）：AddGreatWorkSlots / Culture::
// IsValidObjectForSlot / AI::GreatWorkBuildingInfo、AI ValueManager / Effects::
// AdjustExtraGreatWorkSlots::Initialize（[1]），以及 Culture::Reset、Religion::
// GetNearestRelicSlot、Congress ResolutionGreatWorkObjectTarget 族、AI 计算、
// AdjustGreatWorkObject* 效果等大量 ObjectType 使用点（[3]）。
//
// 线程模型：OnLoad 与 detour 均在游戏主线程，缓存表无锁。

#include "pch.h"
#include "HookAPI.h"
#include "GreatWorkTypeLookup.h"
#include "../../Common/GameCoreResolve.h"

#include <cstdint>
#include <unordered_map>

namespace {

// ---------------------------------------------------------------------------
// 目标描述
// ---------------------------------------------------------------------------
constexpr HookInfo kTargetNameToSlot = HookName(
	"GameCore::Definition::Utility::NameToGreatWorkSlotType",
	"81 F9 C9 7B 6C 2F 7F 45 74 3C 81 F9 48 C8 BF 8A");

constexpr HookInfo kTargetNameToObject = HookName(
	"GameCore::Definition::Utility::NameToGreatWorkObjectType",
	"81 F9 A8 B7 AF 05 7F 54 74 4B 81 F9 7F D4 69 88");

// 数据驱动解析器：只认修饰名（demangled 名里的 GetIndex_25/_26 序号是 IDA 的编号，
// 不属于引擎符号，跨版本会变）。
constexpr HookInfo kTargetGetIndexSlot = HookName(
	"?GetIndex@Types@GameCore@@YA?AW4GreatWorkSlotTypes@2@W432@@Z");

constexpr HookInfo kTargetGetIndexObject = HookName(
	"?GetIndex@Types@GameCore@@YA?AW4GreatWorkObjectTypes@2@W432@@Z");

// 序言校验已移除（入口可能被其它插件改写）。这两个常量仅为兼容
// ResolveVerified 的参数签名而保留，不再参与任何判定。
constexpr uint8_t kSigNameToSlot[6]   = { 0x81, 0xF9, 0xC9, 0x7B, 0x6C, 0x2F };
constexpr uint8_t kSigNameToObject[6] = { 0x81, 0xF9, 0xA8, 0xB7, 0xAF, 0x05 };

// 缓存上限：正常游戏里巨作类型总数 < 200，超过这个数说明哈希空间异常。
constexpr size_t kMaxCacheEntries = 4096;

typedef int (__fastcall* ResolveFn)(int hashOrIndex);

int __fastcall Hook_NameToGreatWorkSlotType(int hash);
int __fastcall Hook_NameToGreatWorkObjectType(int hash);

struct Target {
	const char* label;
	const HookInfo* nameToInfo;
	const HookInfo* getIndexInfo;
	const uint8_t* sigNameTo;
	size_t sigNameToLen;
	void* detour;
	ResolveFn base_NameTo = nullptr;    // 原硬编码 switch（trampoline）
	ResolveFn base_GetIndex = nullptr;  // 数据驱动解析器（直接调用，不挂钩）
	std::unordered_map<int, int> cache; // 哈希 -> 类型索引
};

Target s_targets[2] = {
	{ "GreatWorkSlotType", &kTargetNameToSlot, &kTargetGetIndexSlot,
	  kSigNameToSlot, sizeof(kSigNameToSlot), reinterpret_cast<void*>(&Hook_NameToGreatWorkSlotType) },
	{ "GreatWorkObjectType", &kTargetNameToObject, &kTargetGetIndexObject,
	  kSigNameToObject, sizeof(kSigNameToObject), reinterpret_cast<void*>(&Hook_NameToGreatWorkObjectType) },
};

HookAPI* g_api = nullptr;

// 解析主路径：缓存表 -> 数据驱动解析器 -> 原硬编码 switch。
// trampoline 缺席时安全降级：GetIndex 可独立给出正确答案（它不依赖本代 hook），
// 两边都没有才 -1（= 原版对未知类型的回答，不会回跳已卸载代码）。
int ResolveHash(Target& t, int hash) {
	auto it = t.cache.find(hash);
	if (it != t.cache.end()) {
		return it->second;                    // 已有：直接读取
	}

	int v = -1;
	if (ResolveFn resolver = t.base_GetIndex) {
		v = resolver(hash);                   // 数据库定义集合（索引透传 / 哈希查询）
	}
	if (v == -1) {
		if (ResolveFn orig = t.base_NameTo) { // 回退原硬编码 switch
			v = orig(hash);
		}
	}
	if (v != -1 && t.cache.size() < kMaxCacheEntries) {
		t.cache.emplace(hash, v);             // 新类型首次解析成功即入表
	}
	return v;
}

int __fastcall Hook_NameToGreatWorkSlotType(int hash) {
	return ResolveHash(s_targets[0], hash);
}

int __fastcall Hook_NameToGreatWorkObjectType(int hash) {
	return ResolveHash(s_targets[1], hash);
}

} // namespace

// ---------------------------------------------------------------------------
// 入口 / 出口
// ---------------------------------------------------------------------------

void GreatWorkTypeLookup_Init(HookAPI* api) {
	if (!api) return;
	g_api = api;

	for (Target& t : s_targets) {
		t.base_NameTo = nullptr;
		t.base_GetIndex = nullptr;
		t.cache.clear();   // 定义索引跨代不稳定，整代表作废

		// 数据驱动解析器：只靠修饰名精确匹配。
		// 不做序言校验 —— 入口可能被其它插件 hook/patch 改写，比对会误判。
		// 详见 Common/GameCoreResolve.h 顶部说明。
		void* getIndex = gc::Resolve<void*>(api, kHookImageGameCoreXP2, *t.getIndexInfo, 16);
		if (!getIndex) {
			api->Log("[GreatWorkTypeLookup] %s: data-driven resolver unavailable"
				" - falling back to the hardcoded table only", t.label);
		} else {
			t.base_GetIndex = reinterpret_cast<ResolveFn>(getIndex);
		}

		void* nameTo = gc::ResolveVerified<void*>(api, kHookImageGameCoreXP2, *t.nameToInfo,
			t.sigNameTo, t.sigNameToLen, t.label);
		if (!nameTo) continue;

		if (!api->InstallHook(nameTo, t.detour, reinterpret_cast<void**>(&t.base_NameTo))) {
			api->Log("[GreatWorkTypeLookup] %s: InstallHook failed", t.label);
			continue;
		}
		api->Log("[GreatWorkTypeLookup] %s hooked; resolver=%s (modded types now resolvable)",
			t.label, t.base_GetIndex ? "ok" : "missing");
	}
}

void GreatWorkTypeLookup_Shutdown() {
	// 钩子由 Loader 按 owner 回滚；置空 trampoline 后旧路径再进 detour 只会走
	// 「数据驱动 / 返回 -1」分支，不会回跳已卸载代码。
	for (Target& t : s_targets) {
		t.base_NameTo = nullptr;
		t.base_GetIndex = nullptr;
		t.cache.clear();
	}
	g_api = nullptr;
}
