// GameCoreResolve.h —— GameCore 侧统一的"兼容性取址"工具（header-only）
//
// 背景（docs/PLUGIN_GUIDE.md §5）：游戏每次更新都会让 .text 里的函数整体位移。
// 本工程的所有目标函数一律按下面的顺序解析，任何一步失败都**干净放弃该功能**，
// 绝不拿旧 RVA 硬猜：
//
//   1) 主路径：符号名（demangled 或 MSVC 修饰名）—— 走 Loader 的 PdataMatch
//      跨版本对齐，版本一致时是恒等映射；
//   2) 兜底：AOB 特征码 —— 只有在该特征串于当前 IDB 内**唯一命中**时才附带
//      （见各模块的 kAob* 常量注释），避免"兜底兜到别的函数上"；
//   3) 装钩前再用 PrologueMatches 校验目标处字节形态，对不上就跳过并记日志。
//
// 为什么不再用裸 RVA：ResolveByRva 只有在 Loader 的 sidecar 与当前映像对得上时
// 才有意义；一旦对不上（kHookApiDbNoContext）它会**原样透传**旧 RVA，直接把钩子
// 装到错误的函数上。符号名 + AOB 是更安全的写法。
//
// 线程模型：本文件只做地址解析，全部在 OnLoad（游戏线程、GameCore 已加载）内完成。

#pragma once

#include "HookAPI.h"

#include <cstdint>
#include <cstring>

namespace gc {

// 解析一条 HookInfo（主路径失败自动走 AOB 兜底），并把结果转成目标函数类型。
// span = 要求可读的字节数，用于让 Loader 顺手校验目标确实可读。
template <typename T>
inline T Resolve(HookAPI* api, HookImageId image, const HookInfo& info, size_t span = 16) {
	if (!api) return nullptr;
	return reinterpret_cast<T>(ResolveByHookInfo(api, image, info, span));
}

// 校验目标处的前导字节形态。游戏更新后若符号库把名字对到了别的函数上，
// 这一步能拦住（符号库跨版本对齐本身有失败概率）。
inline bool PrologueMatches(const void* target, const uint8_t* expect, size_t len) {
	return target && expect && len > 0 && std::memcmp(target, expect, len) == 0;
}

// 组合动作：解析 + 校验前导字节。任一失败返回 nullptr 并（可选）打日志。
template <typename T>
inline T ResolveVerified(HookAPI* api, HookImageId image, const HookInfo& info,
	const uint8_t* sig, size_t sigLen, const char* label) {
	T fn = Resolve<T>(api, image, info, 16);
	if (!fn) {
		if (api && label) api->Log("[%s] target not resolvable (symbol + aob both failed)", label);
		return nullptr;
	}
	if (sig && sigLen && !PrologueMatches(reinterpret_cast<const void*>(fn), sig, sigLen)) {
		if (api && label) api->Log("[%s] prologue mismatch - game updated? feature skipped", label);
		return nullptr;
	}
	return fn;
}

} // namespace gc
