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
	int slotIndex;        // 原记录中的绝对槽位下标（保留，用于日志/兜底）
	int nthOfType;        // 该 slotType 在原记录中第几次出现（0-based）
};

struct PendingGreatWork {
	bool                     valid = false;
	int                      buildingType = -1;
	std::vector<PendingSlot> slots;
	std::vector<int>         shape;   // 重建前每个槽的 slotType 序列（按绝对下标）
};

PendingGreatWork g_pending;

void ResetPending() {
	g_pending.valid = false;
	g_pending.buildingType = -1;
	g_pending.slots.clear();
	g_pending.shape.clear();
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

	// 记录完整 shape（每个槽的 slotType），并给每个有效槽算"同类型内第几个"。
	std::vector<int> typeList;
	typeList.reserve(static_cast<size_t>(num));
	char* sp = static_cast<char*>(slots);
	for (int i = 0; i < num; ++i, sp += SLOT_SIZE) {
		typeList.push_back(*reinterpret_cast<int*>(sp + OFF_SLOT_TYPE));
	}
	g_pending.shape = typeList;

	sp = static_cast<char*>(slots);
	for (int i = 0; i < num; ++i, sp += SLOT_SIZE) {
		const int gwIndex = *reinterpret_cast<int*>(sp + OFF_SLOT_INDEX);
		if (gwIndex != kEmptySlot) {
			const int st = typeList[i];
			int nth = 0;
			for (int k = 0; k < i; ++k) {
				if (typeList[k] == st) ++nth;
			}
			g_pending.slots.push_back(PendingSlot{ st, gwIndex, i, nth });
		}
	}
	if (g_pending.slots.empty()) return;

	g_pending.valid = true;
	g_pending.buildingType = buildType;
}

// 把暂存内容按 slotType 回填到空槽上。只写 greatWorkIndex，不动任何结构。
int RestorePendingSlots(void* buildings, int buildType) {
	if (!g_pending.valid || g_pending.buildingType != buildType) return 0;
	if (g_pending.slots.empty()) return 0;

	char* info = FindFirstInfo(buildings, buildType);
	if (!info) return 0;

	void* slots = *reinterpret_cast<void**>(info + OFF_INFO_SLOTS);
	const int num = *reinterpret_cast<int*>(info + OFF_INFO_NUM);
	if (!slots || num <= 0 || num > kMaxSanityCount) return 0;

	// 回填策略（两级）：
	//
	//  一级 —— 若重建后的槽【数量与 slotType 序列完全一致】，直接按绝对下标还原。
	//          这是最常见的情况（改槽数只是在末尾增删额外槽），最精确，
	//          同种巨作并存时也不会串位。
	//
	//  二级 —— 否则（槽序列变了），退化为按 slotType 分组、组内顺序填充。
	//          宁可少填也不错填：只填空槽，且每个暂存项只用一次。
	int restored = 0;

	// ---- 一级：序列完全一致 → 绝对下标还原 ----
	bool sameShape = (num == static_cast<int>(g_pending.shape.size()));
	if (sameShape) {
		char* p = static_cast<char*>(slots);
		for (int i = 0; i < num; ++i, p += SLOT_SIZE) {
			if (*reinterpret_cast<int*>(p + OFF_SLOT_TYPE) != g_pending.shape[i]) {
				sameShape = false;
				break;
			}
		}
	}

	if (sameShape) {
		char* p = static_cast<char*>(slots);
		for (int i = 0; i < num; ++i, p += SLOT_SIZE) {
			if (*reinterpret_cast<int*>(p + OFF_SLOT_INDEX) != kEmptySlot) continue;
			for (size_t k = 0; k < g_pending.slots.size(); ++k) {
				const PendingSlot& ps = g_pending.slots[k];
				if (ps.slotIndex != i) continue;
				*reinterpret_cast<int*>(p + OFF_SLOT_INDEX) = ps.greatWorkIndex;
				++restored;
				break;
			}
		}
		if (g_api) {
			g_api->Log("[GreatWorkPreserve] bt=%d shape-match restored=%d/%d",
				buildType, restored, static_cast<int>(g_pending.slots.size()));
		}
		return restored;
	}

	// ---- 二级：序列变了 → 按 slotType 分组顺序填充 ----
	// 对每种 slotType，收集"重建后该类型的空槽"和"暂存里该类型的项"，按出现顺序配对。
	char* sp = static_cast<char*>(slots);
	for (int i = 0; i < num; ++i, sp += SLOT_SIZE) {
		const int wantType = *reinterpret_cast<int*>(sp + OFF_SLOT_TYPE);

		// 计算当前槽是"该类型内第几个"（重建后）
		int nth = 0;
		for (int k = 0; k < i; ++k) {
			const char* prev = static_cast<char*>(slots) + static_cast<ptrdiff_t>(SLOT_SIZE) * k;
			if (*reinterpret_cast<const int*>(prev + OFF_SLOT_TYPE) == wantType) ++nth;
		}

		if (*reinterpret_cast<int*>(sp + OFF_SLOT_INDEX) != kEmptySlot) continue;

		for (size_t k = 0; k < g_pending.slots.size(); ++k) {
			const PendingSlot& ps = g_pending.slots[k];
			if (ps.slotType != wantType) continue;
			if (ps.nthOfType != nth) continue;

			*reinterpret_cast<int*>(sp + OFF_SLOT_INDEX) = ps.greatWorkIndex;
			++restored;
			break;
		}
	}
	if (g_api) {
		g_api->Log("[GreatWorkPreserve] bt=%d shape-changed restored=%d/%d",
			buildType, restored, static_cast<int>(g_pending.slots.size()));
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
