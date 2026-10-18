// GreatWorkYieldStack.cpp
// 巨作产出加成不叠加修复 —— 从 CivilizationVI_CommunityExtension-1.3.0-beta 的
// Main.cpp（GET_BB_OFFSET=0x11C630 + Hook_BB）移植到 Civ6DllLoader 插件框架。
//
// 相对上一版的改动（兼容性修复）：
//   1) 删除 RegisterGameCoreReady / RegisterGameCoreTeardown（本工程是 GameCore 插件，
//      OnLoad 时 GameCore 已就绪；退局由 Loader 按 owner 回滚钩子）；
//   2) 取址改为「符号名为主 + AOB 兜底 + 前导字节校验」，不再依赖裸 RVA。
//
// 反编译核实（当前 Steam 版 GameCore_XP2_FinalRelease.dll IDB）：
//
//   GameCore::City::Culture::GetGreatWorkYieldChange(this, /*edx*/objType,
//   /*r8d*/yieldType)：
//     从 [this+0x68] 到 [this+0x70] 遍历，元素步长 **0x10**（add rax,10h），
//     每项前三个 int 为 {objectType, yieldType, amount}；首个满足
//     [i]==objType && i[1]==yieldType && dword[i+8]>0 的条目**立即返回**其
//     amount，无命中返回 0。多条同 (objType,yieldType) 来源时只计首条 =
//     加成不叠加的根因。前导字节 48 8B 41 68 4C 8B 49 70（AOB 唯一命中）。
//
//   唯一调用方：City::Buildings::GetBuildingYieldFromGreatWorks（城市巨作产出）。
//
// 与 CE 参考实现的偏差（有意为之）：
//   CE 的 Hook_BB 以 int* 步进 4 字节滑动比较，靠「滑动窗口总会扫过对齐命中」
//   实现求和；本移植按反汇编实证的真实步长 16 字节遍历，语义等价且免去对未
//   对齐三元组的误配可能。amount>0 过滤与逐条求和口径照抄参考实现。
//
// 安全网：begin/end 指针无效或跨度过大（游戏更新导致布局漂移）时回退原函数；
// 仅在没有 trampoline 且布局校验失败时中性返回 0。
//
// 线程模型：OnLoad 与 detour 均在游戏主线程。

#include "pch.h"
#include "HookAPI.h"
#include "GreatWorkYieldStack.h"
#include "../../Common/GameCoreResolve.h"

#include <cstdint>

namespace {

constexpr HookInfo kTargetYieldChange = HookName(
	"GameCore::City::Culture::GetGreatWorkYieldChange",
	"48 8B 41 68 4C 8B 49 70 49 3B C1 74 1A 39 10 75 0C");

constexpr uint8_t kSigYieldChange[8] = { 0x48, 0x8B, 0x41, 0x68, 0x4C, 0x8B, 0x49, 0x70 };

// Culture 加成表元素：{int objectType; int yieldType; int amount; int _pad;} = 16 字节
constexpr size_t OFF_BEGIN = 0x68;
constexpr size_t OFF_END   = 0x70;
constexpr size_t ELEM_SIZE = 16;
constexpr ptrdiff_t MAX_SPAN_BYTES = 1 << 20;   // 合理上界远小于此；超出视为布局异常

typedef int (__fastcall* GetGreatWorkYieldChangeFn)(void* cityCulture, int objectType, int yieldType);

HookAPI* g_api = nullptr;
GetGreatWorkYieldChangeFn g_origYieldChange = nullptr;

int __fastcall Hook_GetGreatWorkYieldChange(void* cityCulture, int objectType, int yieldType) {
	if (!cityCulture) {
		GetGreatWorkYieldChangeFn orig = g_origYieldChange;
		return orig ? orig(cityCulture, objectType, yieldType) : 0;
	}

	const char* begin = *reinterpret_cast<const char**>(static_cast<char*>(cityCulture) + OFF_BEGIN);
	const char* end   = *reinterpret_cast<const char**>(static_cast<char*>(cityCulture) + OFF_END);
	if (!begin || !end || end < begin || (end - begin) > MAX_SPAN_BYTES) {
		// 布局异常（版本漂移/脏指针）：能回退则按原版「首条即返」语义，否则中性 0
		GetGreatWorkYieldChangeFn orig = g_origYieldChange;
		return orig ? orig(cityCulture, objectType, yieldType) : 0;
	}

	int total = 0;
	for (const char* p = begin; p + 12 <= end; p += ELEM_SIZE) {
		const int* entry = reinterpret_cast<const int*>(p);
		if (entry[0] == objectType && entry[1] == yieldType && entry[2] > 0) {
			total += entry[2];               // 全部匹配条目求和 = 叠加修复
		}
	}
	return total;
}

} // namespace

void GreatWorkYieldStack_Init(HookAPI* api) {
	if (!api) return;
	g_api = api;
	g_origYieldChange = nullptr;

	void* target = gc::ResolveVerified<void*>(api, kHookImageGameCoreXP2, kTargetYieldChange,
		kSigYieldChange, sizeof(kSigYieldChange), "GreatWorkYieldStack");
	if (!target) return;

	if (!api->InstallHook(target, reinterpret_cast<void*>(&Hook_GetGreatWorkYieldChange),
			reinterpret_cast<void**>(&g_origYieldChange))) {
		api->Log("%s", "[GreatWorkYieldStack] InstallHook failed");
		return;
	}
	api->Log("%s", "[GreatWorkYieldStack] hooked GetGreatWorkYieldChange; great work yields now stack");
}

void GreatWorkYieldStack_Shutdown() {
	// 置空 trampoline：detour 只依赖内存布局计算，若连布局校验都不过则中性返回 0，
	// 不会回跳已卸载代码。
	g_origYieldChange = nullptr;
	g_api = nullptr;
}
