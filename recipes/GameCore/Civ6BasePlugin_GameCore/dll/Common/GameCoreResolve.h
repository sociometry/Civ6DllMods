// GameCoreResolve.h —— GameCore 侧统一的"兼容性取址"工具（header-only）
//
// 背景（docs/PLUGIN_GUIDE.md §5）：游戏每次更新都会让 .text 里的函数整体位移。
// 本工程的所有目标函数一律按下面的顺序解析，任何一步失败都**干净放弃该功能**，
// 绝不拿旧 RVA 硬猜：
//
//   1) 主路径：符号名（demangled 或 MSVC 修饰名）—— 走 Loader 的 PdataMatch
//      跨版本对齐，版本一致时是恒等映射；
//   2) 兜底：AOB 特征码 —— 只有在该特征串于当前 IDB 内**唯一命中**时才附带
//      （见各模块的 kAob* 常量注释），避免"兜底兜到别的函数上"。
//
// ============================================================================
// 【关于函数序言校验（Prologue Check）：慎用，本工程已停用】
//
// 术语：「函数序言」= function prologue，指函数入口处的前几条指令
//       （建立栈帧、保存寄存器等）。旧文档里的"前导字节"不是规范译法。
//
// 旧版本在解析出地址后，会把目标序言的 N 个字节与硬编码期望值比对，
// 对不上就判定"游戏更新了"并放弃功能。它在多插件环境里有明显副作用：
//
//   · 序言是"函数入口的机器码"。只要有人在目标函数上下过 hook / patch，
//     入口就会被改写成 jmp（E9 xx xx xx xx）或热补丁形态，
//     于是后加载的插件读到的已不是原始字节，校验必然失败。
//   · 实测事故：本工程的一个模块先对 AddGreatWorkToSlot 装钩，
//     另一个模块随后做序言校验，读到被改写的入口 → 判定 mismatch →
//     整个 Lua API 组被标记为"符号不完整"并全部返回 false。
//   · 顺序依赖：谁能通过校验取决于插件加载顺序，属不可控的隐性耦合。
//   · 失败原因不可辨：符号对齐错 vs 入口被改写，表象都是 mismatch。
//
// 因此：**序言校验已从本工程全部停用**。
// 判断"符号是否指向正确的函数"时，优先改用：
//   1) 符号名（首选，跨版本由 Loader 的 PdataMatch 对齐）；
//   2) 在当前 IDB 内唯一命中的 AOB（作为兜底）；
//   3) 调用前的轻量运行期自检（返回值范围、空指针、枚举合法性）。
//
// 例外：如果某个目标【确定不会被其它插件 hook】、且特征码足够长且在当前 IDB
// 内唯一，那么加序言校验是合理的 —— 但必须做到"失败只降级、不连带"，
// 即序言不匹配时只关掉这一项功能，不要据此关掉整组功能。
// 详见 docs/PLUGIN_GUIDE.md §5.9。
// ============================================================================
//
// 为什么不用裸 RVA：ResolveByRva 只有在 Loader 的 sidecar 与当前映像对得上时
// 才有意义；一旦对不上（kHookApiDbNoContext）它会**原样透传**旧 RVA，直接把钩子
// 装到错误的函数上。符号名 + AOB 是更安全的写法。
//
// 线程模型：本文件只做地址解析，全部在 OnLoad（游戏线程、GameCore 已加载）内完成。

#pragma once

#include "HookAPI.h"

#include <cstdint>

namespace gc {

// 解析一条 HookInfo（主路径失败自动走 AOB 兜底），并把结果转成目标函数类型。
// span = 要求可读的字节数，用于让 Loader 顺手校验目标确实可读。
template <typename T>
inline T Resolve(HookAPI* api, HookImageId image, const HookInfo& info, size_t span = 16) {
	if (!api) return nullptr;
	return reinterpret_cast<T>(ResolveByHookInfo(api, image, info, span));
}

// 兼容旧调用点的包装：解析 +（已停用的）序言校验参数。
//
// 参数 sig / sigLen 仍然保留，纯粹为了不改动各模块的调用代码；
// 它们现在【完全不被使用】。新代码请直接调用 Resolve<T>()。
template <typename T>
inline T ResolveVerified(HookAPI* api, HookImageId image, const HookInfo& info,
	const uint8_t* /*sig*/, size_t /*sigLen*/, const char* label) {
	T fn = Resolve<T>(api, image, info, 16);
	if (!fn && api && label) {
		api->Log("[%s] target not resolvable (symbol + aob both failed)", label);
	}
	return fn;
}

} // namespace gc
