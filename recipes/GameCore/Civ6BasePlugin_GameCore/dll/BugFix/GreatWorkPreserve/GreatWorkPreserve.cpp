// GreatWorkPreserve.cpp
// 额外巨作槽变更导致巨作丢失修复。
//
// 反编译核实（GameCore_XP2_FinalRelease.dll）：
//
//   GameCore::City::Buildings（sizeof = 0x170）
//     +0x0C8  GreatWorkInfo*  greatWorkBuildings          （元素 0x20 字节）
//     +0x0D8  int             greatWorkBuildingsCount
//
//   GreatWorkInfo（0x20）：
//     +0x00 int BuildingType
//     +0x08 GreatWorkSlot* GreatWorks
//     +0x10 int GreatWorkSlotCount
//     +0x18 int GreatWorkNum
//
//   GreatWorkSlot（0x08）：
//     +0x00 GreatWorkSlotTypes slotType
//     +0x04 GreatWorkListIndex greatWorkIndex   （-1 = 空）
//
// 缺陷（ChangeExtraGreatWorkSlots 尾部成对调用，见 IDA 反汇编）：
//   RemoveGreatWorkSlots(this, buildType);   // 删掉该建筑的记录，槽数组一并释放
//   AddGreatWorkSlots(this, buildType);      // 只按定义重建**空槽**，greatWorkIndex 全 -1
//   两次调用之间没有任何数据传递 —— 已放置的巨作全部丢失且不归还玩家。
//   触发场景：AdjustExtraGreatWorkSlots 效果（政策卡 / 奇观 / 伟人）改变槽数。
//
// 修法（单个钩子）：
//   钩 ChangeExtraGreatWorkSlots —— 它是"改槽数丢巨作"的唯一入口。
//     入口：把目标 buildType 的有效槽内容深拷贝到插件私有暂存区
//           （std::vector，不持有游戏堆指针）；
//     调用原函数：内部 Remove → Add，重建出一组全空槽；
//     出口：把暂存内容按槽位回填到空槽上。
//
//   为什么只钩这一个：ChangeExtraGreatWorkSlots 是唯一出口（无提前 return），
//   一次"入口存 + 出口填"即可完整覆盖。若改钩被它内部调用的那两个 private 函数
//   （Add/RemoveGreatWorkSlots），会误伤它们的其它调用者：
//     · RemoveBuilding / RemoveAllBuildings —— 拆建筑，之后不会再 Add，
//       抓的暂存会一直残留，等下一个同 buildType 的 Add 误消费；
//     · SetBuildingLocation —— 建新建筑，本来就没有旧巨作，"恢复"多余。
//
// ============================================================================
// 设计红线（踩过的坑，勿改）：
//
//   1) **绝不改变 greatWorkBuildings 数组的结构**。
//      不要做"去重 / 合并重复项 / 移动元素 / 修改 count"这类操作。
//      实测教训：曾用 memcpy 把尾项搬到中间再 --count，结果导致
//      MOVE_GREAT_WORK 期间按数组下标定位的写入全部错位，
//      表现为"移动巨作后其它巨作位置乱掉"。
//      本模块只做"读出来 → 原函数 → 按位回填"，不碰数组形状。
//
//   2) **只写 greatWorkIndex，绝不改 slotType、不改数组指针、不改 count**。
//
//   3) **回填只填空槽（greatWorkIndex == -1）**，不覆盖已有内容。
//      这样即便与 MOVE_GREAT_WORK 交错，也不会把用户刚移动的巨作冲掉。
//
//   4) 暂存区只在【一次 ChangeExtraGreatWorkSlots 调用内】有效：
//      入口 Reset 后写入，出口用完立刻清空。不存在跨调用的残留或误消费。
// ============================================================================
//
// 线程模型：OnLoad 与 detour 均在游戏主线程。

#include "pch.h"
#include "HookAPI.h"
#include "GreatWorkPreserve.h"
#include "../../Common/GameCoreResolve.h"

#include <cstdint>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// 目标
// ---------------------------------------------------------------------------

// ChangeExtraGreatWorkSlots（public，RVA 0x109C50）
//
// 选它而不是 Add/RemoveGreatWorkSlots 的原因：
//   它是【唯一】真正导致"改槽数丢巨作"的入口（政策卡 / 奇观 / 伟人触发的
//   AdjustExtraGreatWorkSlots 效果都汇到这里），内部成对调用
//   RemoveGreatWorkSlots → AddGreatWorkSlots。
//   而 Add/RemoveGreatWorkSlots 还有别的调用者（SetBuildingLocation 建新建筑、
//   RemoveBuilding / RemoveAllBuildings 拆建筑），hook 那两个会误伤：
//     · 建新建筑时本来就没有旧巨作，"恢复"是多余的；
//     · 拆建筑时 Remove 之后的暂存会一直残留，等下个同 buildType 的 Add 误消费。
//   钩这一个 public 函数，入口存一份、出口填回去，语义最干净。
constexpr HookInfo kTargetChange = HookName(
	"GameCore::City::Buildings::ChangeExtraGreatWorkSlots",
	"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 48 8D B9 E0 00 00 00");

constexpr uint8_t kSigChange[20] = {
	0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10,
	0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x30
};

// ---------------------------------------------------------------------------
// 结构体偏移
// ---------------------------------------------------------------------------
constexpr size_t OFF_GW_DATA    = 0x0C8;
constexpr size_t OFF_GW_COUNT   = 0x0D8;
constexpr size_t GWINFO_SIZE    = 0x20;
constexpr size_t OFF_INFO_TYPE  = 0x00;
constexpr size_t OFF_INFO_SLOTS = 0x08;
constexpr size_t OFF_INFO_NUM   = 0x18;
constexpr size_t SLOT_SIZE      = 0x08;
constexpr size_t OFF_SLOT_TYPE  = 0x00;
constexpr size_t OFF_SLOT_INDEX = 0x04;

constexpr int kEmptySlot      = -1;
constexpr int kMaxSanityCount = 4096;

HookAPI* g_api = nullptr;

typedef void (__fastcall* ChangeExtraGreatWorkSlotsFn)(
	void* buildings, int buildType, int slotType, int changeNum);
ChangeExtraGreatWorkSlotsFn g_origChange = nullptr;

// ---------------------------------------------------------------------------
// 暂存区
// ---------------------------------------------------------------------------
struct PendingSlot {
	int slotType;
	int greatWorkIndex;
	int slotIndex;        // 原记录中的绝对槽位下标（第一轮原位回归用）
};

struct PendingGreatWork {
	bool                     valid = false;
	int                      buildingType = -1;
	std::vector<PendingSlot> slots;
};

PendingGreatWork g_pending;

void ResetPending() {
	g_pending.valid = false;
	g_pending.buildingType = -1;
	g_pending.slots.clear();
}

// 在 greatWorkBuildings 里找指定 buildType 的第一条记录（只读，不改数组）。
char* FindFirstInfo(void* buildings, int buildType) {
	if (!buildings) return nullptr;
	char* b = static_cast<char*>(buildings);

	void* data = *reinterpret_cast<void**>(b + OFF_GW_DATA);
	const int count = *reinterpret_cast<int*>(b + OFF_GW_COUNT);
	if (!data || count <= 0 || count > kMaxSanityCount) return nullptr;

	char* p = static_cast<char*>(data);
	for (int i = 0; i < count; ++i, p += GWINFO_SIZE) {
		if (*reinterpret_cast<int*>(p + OFF_INFO_TYPE) == buildType) return p;
	}
	return nullptr;
}

// 抓取指定 buildType 的有效槽内容。在 ChangeExtraGreatWorkSlots 入口、
// 即 Remove+Add 重建【之前】调用。
//
// 由于只 hook ChangeExtraGreatWorkSlots 一个函数，不存在"其它调用方的残留"问题：
// 每次进入本函数都先 ResetPending()，栈上独占一份，出口用完即弃。
void CapturePendingSlots(void* buildings, int buildType) {
	ResetPending();

	char* info = FindFirstInfo(buildings, buildType);
	if (!info) return;

	void* slots = *reinterpret_cast<void**>(info + OFF_INFO_SLOTS);
	const int num = *reinterpret_cast<int*>(info + OFF_INFO_NUM);
	if (!slots || num <= 0 || num > kMaxSanityCount) return;

	// 只记录有效槽：slotType + greatWorkIndex + 原绝对下标。
	// 绝对下标用于出口的第一轮"原位回归"（改槽数往往只在末尾增删，位置不变）。
	char* sp = static_cast<char*>(slots);
	for (int i = 0; i < num; ++i, sp += SLOT_SIZE) {
		const int gwIndex = *reinterpret_cast<int*>(sp + OFF_SLOT_INDEX);
		if (gwIndex == kEmptySlot) continue;
		const int st = *reinterpret_cast<int*>(sp + OFF_SLOT_TYPE);
		g_pending.slots.push_back(PendingSlot{ st, gwIndex, i });
	}
	if (g_pending.slots.empty()) return;

	g_pending.valid = true;
	g_pending.buildingType = buildType;
}

// 把暂存内容回填到空槽上。只写 greatWorkIndex，不动任何结构。
//
// 分两轮，目的都是"尽量把能留下的留下"：
//
//   第一轮 —— 原位回归。
//              重建后的槽位下标若仍然存在、且 slotType 与暂存项一致、且该槽为空，
//              就把巨作放回原来的位置。这是最精确的恢复，同种巨作并存也不会串位。
//
//   第二轮 —— 兼容空槽顺延。
//              第一轮没放回去的（典型场景：缩容导致槽位前移 / 该位置已被占用），
//              在剩余的空槽里找同 slotType 的依次填入，每个暂存项只用一次，
//              每个槽只填一次。
//
// 只在【第二轮也填不下】时才真正丢数据 —— 那是容量真的不够，会打日志说明，
// 而不是把"有空间"和"没空间"两种情况都静默丢弃。
//
// 红线：绝不覆盖已有内容（只在 greatWorkIndex == kEmptySlot 的槽上写）。
int RestorePendingSlots(void* buildings, int buildType) {
	if (!g_pending.valid || g_pending.buildingType != buildType) return 0;
	if (g_pending.slots.empty()) return 0;

	char* info = FindFirstInfo(buildings, buildType);
	if (!info) return 0;

	void* slots = *reinterpret_cast<void**>(info + OFF_INFO_SLOTS);
	const int num = *reinterpret_cast<int*>(info + OFF_INFO_NUM);
	if (!slots || num <= 0 || num > kMaxSanityCount) return 0;

	char* base = static_cast<char*>(slots);

	// 每个暂存项是否已恢复；每个槽是否已被本次填过。
	std::vector<bool> done(g_pending.slots.size(), false);
	std::vector<bool> slotUsed(static_cast<size_t>(num), false);

	int restored = 0;

	// ---- 第一轮：原位回归 ----
	for (size_t k = 0; k < g_pending.slots.size(); ++k) {
		const PendingSlot& ps = g_pending.slots[k];
		const int idx = ps.slotIndex;
		if (idx < 0 || idx >= num) continue;          // 该位置已不存在（缩容）
		if (slotUsed[static_cast<size_t>(idx)]) continue;

		char* sp = base + static_cast<ptrdiff_t>(SLOT_SIZE) * idx;
		if (*reinterpret_cast<int*>(sp + OFF_SLOT_TYPE) != ps.slotType) continue;  // 槽类型变了
		if (*reinterpret_cast<int*>(sp + OFF_SLOT_INDEX) != kEmptySlot) continue;  // 已被占

		*reinterpret_cast<int*>(sp + OFF_SLOT_INDEX) = ps.greatWorkIndex;
		slotUsed[static_cast<size_t>(idx)] = true;
		done[k] = true;
		++restored;
	}

	const int inPlace = restored;

	// ---- 第二轮：剩余空槽顺延填 ----
	// 按【槽类型】分组配对：对每个还没恢复的暂存项，找第一个类型相同、且仍为空的槽。
	// 这样即便槽位整体前移、或同类型槽之间有空缺，也能把巨作放下。
	if (restored < static_cast<int>(g_pending.slots.size())) {
		for (size_t k = 0; k < g_pending.slots.size(); ++k) {
			if (done[k]) continue;
			const PendingSlot& ps = g_pending.slots[k];

			for (int i = 0; i < num; ++i) {
				if (slotUsed[static_cast<size_t>(i)]) continue;
				char* sp = base + static_cast<ptrdiff_t>(SLOT_SIZE) * i;
				if (*reinterpret_cast<int*>(sp + OFF_SLOT_TYPE) != ps.slotType) continue;
				if (*reinterpret_cast<int*>(sp + OFF_SLOT_INDEX) != kEmptySlot) continue;

				*reinterpret_cast<int*>(sp + OFF_SLOT_INDEX) = ps.greatWorkIndex;
				slotUsed[static_cast<size_t>(i)] = true;
				done[k] = true;
				++restored;
				break;
			}
		}
	}

	const int total = static_cast<int>(g_pending.slots.size());
	if (g_api) {
		g_api->Log("[GreatWorkPreserve] bt=%d in-place=%d filled=%d restored=%d/%d",
			buildType, inPlace, restored - inPlace, restored, total);
	}

	// 真正填不下才报告丢失（容量不足），并且指明是哪几个 index。
	if (restored < total && g_api) {
		for (size_t k = 0; k < g_pending.slots.size(); ++k) {
			if (done[k]) continue;
			g_api->Log("[GreatWorkPreserve] bt=%d LOST gwIndex=%d slotType=%d (no compatible empty slot)",
				buildType, g_pending.slots[k].greatWorkIndex, g_pending.slots[k].slotType);
		}
	}
	return restored;
}

// ---------------------------------------------------------------------------
// Detours
// ---------------------------------------------------------------------------

// 唯一的 detour：包住整个 ChangeExtraGreatWorkSlots。
//
//   ┌─ 入口：把该 buildType 当前的有效巨作槽内容抓进暂存
//   │   （此时 Remove+Add 还没执行，数据完好）
//   ├─ 调原函数：它内部 RemoveGreatWorkSlots → AddGreatWorkSlots，
//   │   重建出一组全空的槽（原来的巨作在这里被丢掉）
//   └─ 出口：把暂存内容填回空槽
//
// 由于该函数只有一个出口（无提前 return），一次"入口存 + 出口填"即可完整覆盖。
void __fastcall Hook_ChangeExtraGreatWorkSlots(
	void* buildings, int buildType, int slotType, int changeNum) {
	if (!g_origChange) return;

	// changeNum == 0 时引擎什么都不做（内部 Remove/Add 都不会被调用），直接放行，
	// 避免无谓的扫描。
	if (changeNum == 0) {
		g_origChange(buildings, buildType, slotType, changeNum);
		return;
	}

	CapturePendingSlots(buildings, buildType);          // 入口：保存
	const bool hasPending = g_pending.valid;

	g_origChange(buildings, buildType, slotType, changeNum);   // 原逻辑

	int restored = 0;
	if (hasPending) {
		restored = RestorePendingSlots(buildings, buildType);   // 出口：回填
	}
	ResetPending();

	if (g_api && restored > 0) {
		g_api->Log("[GreatWorkPreserve] buildingType=%d slotType=%d change=%+d restored=%d",
			buildType, slotType, changeNum, restored);
	}
}

} // namespace

void GreatWorkPreserve_Init(HookAPI* api) {
	if (!api) return;
	g_api = api;
	g_origChange = nullptr;
	ResetPending();

	void* target = gc::ResolveVerified<void*>(api, kHookImageGameCoreXP2, kTargetChange,
		kSigChange, sizeof(kSigChange), "GreatWorkPreserve/ChangeExtraGreatWorkSlots");
	if (!target) {
		api->Log("%s", "[GreatWorkPreserve] ChangeExtraGreatWorkSlots unresolved - skipped");
		return;
	}

	if (!api->InstallHook(target, reinterpret_cast<void*>(&Hook_ChangeExtraGreatWorkSlots),
			reinterpret_cast<void**>(&g_origChange))) {
		api->Log("%s", "[GreatWorkPreserve] InstallHook(ChangeExtraGreatWorkSlots) failed");
		return;
	}

	api->Log("%s",
		"[GreatWorkPreserve] active: extra great work slot changes keep existing great works");
}

void GreatWorkPreserve_Shutdown() {
	ResetPending();
	g_origChange = nullptr;
	g_api = nullptr;
}