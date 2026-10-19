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

// 支持 "??" 通配的版本。value 与 mask 各 len 字节，mask=0xFF 表示该字节必须相等，
// mask=0x00 表示跳过。用于跨过 RIP 相对位移（disp32）这类随重定位变化的字节，
// 让校验能延伸到位移之后的关键字节（例如函数的 retn）。
inline bool PrologueMatchesMasked(const void* target, const uint8_t* value,
	const uint8_t* mask, size_t len) {
	if (!target || !value || !mask || len == 0) return false;
	const auto* bytes = static_cast<const uint8_t*>(target);
	for (size_t i = 0; i < len; ++i) {
		if ((bytes[i] & mask[i]) != (value[i] & mask[i])) return false;
	}
	return true;
}

// 把 "48 8B 05 ?? ?? ?? ?? C3" 这样的文本特征码在编译期转成 value/mask 两个数组。
// 允许 '?' 或 '??' 表示通配，其余必须是两位十六进制。
template <size_t N>
struct AobPattern {
	uint8_t value[N]{};
	uint8_t mask[N]{};
	size_t count = 0;   // 实际字节数（N 是文本长度，比字节数大）
	bool ok = false;
};

constexpr int HexNibble(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

template <size_t N>
constexpr AobPattern<N> ParseAob(const char (&text)[N]) {
	AobPattern<N> out{};
	size_t count = 0;
	int hi = -1;      // 已读到的高半字节；-1 表示本字节尚未开始
	bool wildcard = false;
	for (size_t i = 0; i + 1 < N; ++i) {
		const char c = text[i];
		if (c == ' ' || c == '\t') {
			if (hi >= 0 || wildcard) {
				if (count < N) {
					out.value[count] = wildcard ? 0 : static_cast<uint8_t>(hi << 4);
					out.mask[count] = wildcard ? 0 : 0xFF;
					++count;
				}
				hi = -1;
				wildcard = false;
			}
			continue;
		}
		if (c == '?') { wildcard = true; continue; }
		const int nib = HexNibble(c);
		if (nib < 0) return out;   // 非法字符：ok 保持 false
		if (hi < 0) {
			hi = nib;
		} else {
			if (count < N) {
				out.value[count] = static_cast<uint8_t>((hi << 4) | nib);
				out.mask[count] = 0xFF;
				++count;
			}
			hi = -1;
		}
	}
	if (hi >= 0 || wildcard) {
		if (count < N) {
			out.value[count] = wildcard ? 0 : static_cast<uint8_t>(hi << 4);
			out.mask[count] = wildcard ? 0 : 0xFF;
			++count;
		}
	}
	out.count = count;
	out.ok = count > 0;
	return out;
}

// 用法：
//   constexpr auto kSigGameEdit = gc::ParseAob("48 8B 05 ?? ?? ?? ?? C3");
//   gc::ResolveVerifiedAob<Fn>(api, image, info, "label", kSigGameEdit);

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

// 同上，但前导字节用带通配的文本特征码（会跳过 disp32 之类的可变字节）。
// pattern 以模板参数传入，ParseAob 才能当常量表达式求值。
template <typename T, size_t N>
inline T ResolveVerifiedAob(HookAPI* api, HookImageId image, const HookInfo& info,
	const char* label, const AobPattern<N>& parsed) {
	T fn = Resolve<T>(api, image, info, 16);
	if (!fn) {
		if (api && label) api->Log("[%s] target not resolvable (symbol + aob both failed)", label);
		return nullptr;
	}
	if (parsed.ok && !PrologueMatchesMasked(reinterpret_cast<const void*>(fn),
			parsed.value, parsed.mask, parsed.count)) {
		if (api && label) api->Log("[%s] prologue mismatch - game updated? feature skipped", label);
		return nullptr;
	}
	return fn;
}

} // namespace gc
