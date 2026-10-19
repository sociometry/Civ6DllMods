// UnitDamageCap.cpp
// 单位伤害上限对齐真实 MaxHitPoints —— XP2 专属 BugFix。
//
// ---------------------------------------------------------------------------
// 缺陷
// ---------------------------------------------------------------------------
// XP2 的 GameCore::Unit::Instance::SetDamage（RVA 0x3A5A60）：
//
//   13f3c5a79  mov esi, [rcx+4C0h]                    ; old = m_iDamage
//   13f3c5a82  xor edx, edx                           ; Range 下界 = 0
//   13f3c5a84  mov ecx, r9d                           ; Range 值 = finalDamage
//   13f3c5a87  mov r8d, [rax+1E8h]                    ; Range 上界 = COMBAT_MAX_HIT_POINTS
//   13f3c5a8e  call Utilities::Range
//   13f3c5a93  mov [rsp+clampedDamage], eax
//   13f3c5a97  lea rcx, [rbx+4B0h]                    ; &m_iDamage (FAutoVariable)
//   13f3c5aa9  call MoveAssignmentDo                  ; 赋值并标脏
//   13f3c5ab0  cmp esi, [rbx+4C0h] / jz ...           ; 值变了就发事件
//
// Base 版（0x18029CC40）在 Range 之前多一段：按 hash 0x7D3FC711 从
// unit->m_kProperties(+0x1F0) 取 MaxHitPoints 属性，校验通过就用它的值当上限，
// 取不到才回退全局常量。XP2 把这段删了，直接写死全局值。
//
// ---------------------------------------------------------------------------
// 修法：替换 0x13f3c5a87 这一条 7 字节指令
// ---------------------------------------------------------------------------
//   原：44 8B 80 E8 01 00 00    mov r8d, [rax+1E8h]
//   新：E9 xx xx xx xx 90 90    jmp trampoline; nop; nop
//
// trampoline（本模块 VirtualAlloc 出来的可执行页）：
//   保存 rbx/rcx/rdx/r8/r9/rax 等会被 GetMaxDamage 破坏的寄存器
//   → 调 Unit::Instance::GetMaxDamage(unit)  得到真实上限
//   → 写 r8d
//   → 恢复寄存器
//   → jmp 回 0x13f3c5a8e（call Utilities::Range）
//
// 这样 SetDamage 只跑一遍、只赋值一次，事件也只发一次，且数值是最终值；
// MoveAssignmentDo 的脏标记机制完整保留，联机同步不受影响。
//
// ---------------------------------------------------------------------------
// 定位（已用 tools/rva_to_symbol.exe 核对）
// ---------------------------------------------------------------------------
//   SetDamage        RVA 0x3A5A60   GameCore::Unit::Instance::SetDamage
//   补丁点           RVA 0x3A5A87   SetDamage+0x27，第 7 字节指令的起点
//   回跳点           RVA 0x3A5A8E   SetDamage+0x2E，call Utilities::Range
//   GetMaxDamage     RVA 0x3A39A0   GameCore::Unit::Instance::GetMaxDamage
//
// 线程模型：全部在游戏主线程。

#include "pch.h"
#include "HookAPI.h"
#include "UnitDamageCap.h"
#include "../../Common/GameCoreResolve.h"

#include <asmjit/core.h>
#include <asmjit/x86.h>

#include <cstdint>
#include <cstring>

namespace {

// SetDamage 整个函数：只用于拿基址，实际只补其中一条指令。
constexpr HookInfo kTargetSetDamage = HookName(
	"GameCore::Unit::Instance::SetDamage",
	"48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 40 48 8B 05 ?? ?? ?? ?? 44 8B CA 8B B1 C0 04 00 00");
constexpr uint8_t kSigSetDamage[8] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74 };

constexpr HookInfo kTargetGetMaxDamage = HookName(
	"GameCore::Unit::Instance::GetMaxDamage",
	"48 83 EC 28 48 81 C1 F0 01 00 00 BA 11 C7 3F 7D");
constexpr uint8_t kSigGetMaxDamage[8] = { 0x48, 0x83, 0xEC, 0x28, 0x48, 0x81, 0xC1, 0xF0 };

// 补丁点相对 SetDamage 的偏移，以及要替换的原字节。
constexpr size_t OFF_CLAMP_LOAD = 0x27;   // 0x3A5A87 - 0x3A5A60
constexpr size_t OFF_CLAMP_CALL = 0x2E;   // 0x3A5A8E - 0x3A5A60
// mov r8d, [rax+1E8h]
constexpr uint8_t kOrigClampLoad[7] = { 0x44, 0x8B, 0x80, 0xE8, 0x01, 0x00, 0x00 };

typedef int(__fastcall* GetMaxDamageFn)(void* unit);

HookAPI* g_api = nullptr;
GetMaxDamageFn g_getMaxDamage = nullptr;
uint8_t* g_trampoline = nullptr;
uint8_t* g_clampSite = nullptr;

// trampoline 代码（x64）：
//   进入时 rbx=unit, rax=ms_pkParameters, rcx/rdx/r8/r9 是 Range 的入参。
//   需要：调 GetMaxDamage(rbx) 得 eax=真实上限，写进 r8d，恢复现场后跳回。
//
//   push rcx / rdx / r8 / r9 / rax / rsi / rdi   （GetMaxDamage 会破坏的调用者保存寄存器）
//   mov  rcx, rbx
//   mov  rax, <getMaxDamage>
//   call rax
//   mov  r8d, eax
//   pop  rdi / rsi / rax / r9 / r8 / rdx / rcx
//   jmp  <returnAddr>
//
// 注意：r8 在 callee-saved 意义上不是"必须保存"的，但这里它是我们**要写**的目标，
// 所以只需要最后写入即可，不必先保存再恢复；push/pop 它只为对齐栈。
constexpr size_t kTrampolineSize = 128;

// 生成一条 asmjit 指令；失败时写日志并返回 false。
// 必须逐条检查的原因：asmjit 对非法操作数只写进 emitter 的局部 err，
// rt.add() 照样返回 kErrorOk，那条指令会被**静默丢弃**。
inline bool Emit(asmjit::x86::Assembler& a, asmjit::Error e, int line) {
	if (e == asmjit::kErrorOk) return true;
	if (g_api) {
		g_api->Log("[UnitDamageCap] asmjit emit failed @%d: %s",
			line, asmjit::DebugUtils::errorAsString(e));
	}
	return false;
}

#define UC_EMIT(expr) do { if (!Emit(a, (expr), __LINE__)) return false; } while (0)

// 生成 "jmp rel32"（E9 xx xx xx xx）并以占位 0 先写入，稍后由 PatchJmpRel32 回填。
//
// 为什么不能直接用 a.jmp():
//   实测 a.jmp((uint64_t)addr) 生成 FF 25 02 00 00 00 + 内联指针 = 16 字节的
//   **间接跳转**（jmp qword ptr [rip+2]），语义是"读目标处 8 字节当指针再跳"。
//   用它跳回 SetDamage 会把目标处的机器码当指针解引用，直接崩溃。
//   a.jmp(x86::ptr(addr)) 报 RelocOffsetOutOfRange；a.jmp(Label) 只能指向同一个
//   CodeHolder 内部，跳不到游戏模块的绝对地址。
// 所以：用 embed 写出 E9 + 占位 rel32，拿到最终地址后回填。
inline bool EmitJmpRel32Placeholder(asmjit::x86::Assembler& a) {
	using namespace asmjit;
	const Error e1 = a.embedUInt8(0xE9);
	if (e1 != kErrorOk) return false;
	const Error e2 = a.embedInt32(0);   // 占位，稍后回填
	return e2 == kErrorOk;
}

// 把 jmp 指令的 rel32 回填。site 是该指令在内存中的地址，relOffset 是 rel32
// 相对该指令起始的偏移（恒为 1）。
inline bool PatchJmpRel32(uint8_t* jmpInsn, void* target, const char* what) {
	const int64_t rel = reinterpret_cast<int64_t>(target) -
		(reinterpret_cast<int64_t>(jmpInsn) + 5);
	if (rel > INT32_MAX || rel < INT32_MIN) {
		if (g_api) {
			g_api->Log("[UnitDamageCap] %s: jmp target out of rel32 range (%p -> %p)",
				what, jmpInsn, target);
		}
		return false;
	}
	const int32_t r32 = static_cast<int32_t>(rel);
	std::memcpy(jmpInsn + 1, &r32, 4);
	return true;
}

// 生成 trampoline 与补丁字节。两处的 jmp rel32 都先写占位，
// 由调用方在拿到真实内存地址后回填（out 必须是最终地址）。
//
// 参数：
//   getMax         GetMaxDamage 函数地址
//   out            trampoline 的最终内存地址（已分配好，长度 outSize）
//   outLen         输出：trampoline 实际长度
//   patchOut       输出：补丁字节（长度 = patchLen，尾部 nop 填充）
//   patchLen       补丁字节数（= 被替换的原指令长度）
//   patchSiteAddr  补丁点在内存中的地址（供计算 jmp rel32）
//   returnAddr     跳回地址（SetDamage 里 call Utilities::Range 那条）
bool BuildTrampoline(void* returnAddr, GetMaxDamageFn getMax, uint8_t* out, size_t outSize,
	size_t* outLen, uint8_t* patchOut, size_t patchLen, void* patchSiteAddr) {
	using namespace asmjit;

	JitRuntime rt;
	CodeHolder code;
	code.init(rt.environment());
	x86::Assembler a(&code);

	// 保存易失寄存器（rsi/rdi 在 SetDamage 里正被使用，必须保）
	UC_EMIT(a.push(x86::rcx));
	UC_EMIT(a.push(x86::rdx));
	UC_EMIT(a.push(x86::rax));
	UC_EMIT(a.push(x86::rsi));
	UC_EMIT(a.push(x86::rdi));
	UC_EMIT(a.push(x86::r8));
	UC_EMIT(a.push(x86::r9));
	UC_EMIT(a.sub(x86::rsp, 8));      // 凑齐 64 字节，保持 16 对齐

	UC_EMIT(a.mov(x86::rcx, x86::rbx));            // rcx = unit
	UC_EMIT(a.mov(x86::rax, (uint64_t)getMax));    // rax = GetMaxDamage
	UC_EMIT(a.call(x86::rax));                     // eax = 真实上限
	UC_EMIT(a.mov(x86::r8d, x86::eax));            // r8d = 上限（Range 的上界）

	UC_EMIT(a.add(x86::rsp, 8));
	UC_EMIT(a.pop(x86::r9));
	UC_EMIT(a.pop(x86::r8));
	UC_EMIT(a.pop(x86::rdi));
	UC_EMIT(a.pop(x86::rsi));
	UC_EMIT(a.pop(x86::rax));
	UC_EMIT(a.pop(x86::rdx));
	UC_EMIT(a.pop(x86::rcx));

	// 末尾 jmp 回 SetDamage：先占位
	const size_t trampJmpOff = code.codeSize();
	if (!Emit(a, EmitJmpRel32Placeholder(a), __LINE__)) return false;

	void* fn = nullptr;
	const Error err = rt.add(&fn, &code);
	if (err != kErrorOk || !fn) {
		if (g_api) {
			g_api->Log("[UnitDamageCap] asmjit jit failed: %s",
				DebugUtils::errorAsString(err));
		}
		return false;
	}

	const size_t size = code.codeSize();
	if (size > outSize) {
		if (g_api) {
			g_api->Log("[UnitDamageCap] trampoline too large: %zu > %zu", size, outSize);
		}
		rt.release(fn);
		return false;
	}
	std::memcpy(out, fn, size);
	rt.release(fn);
	*outLen = size;

	// 回填 trampoline 尾部 jmp（out 已是最终地址）
	if (!PatchJmpRel32(out + trampJmpOff, returnAddr, "trampoline")) {
		return false;
	}

	// ---- 补丁字节：jmp patchSiteAddr->out ; nop 填充 ----
	JitRuntime rt2;
	CodeHolder code2;
	code2.init(rt2.environment());
	x86::Assembler a2(&code2);

	if (!Emit(a2, EmitJmpRel32Placeholder(a2), __LINE__)) return false;
	while (code2.codeSize() < patchLen) {
		if (!Emit(a2, a2.nop(), __LINE__)) return false;
	}

	void* fn2 = nullptr;
	const Error err2 = rt2.add(&fn2, &code2);
	if (err2 != kErrorOk || !fn2) {
		if (g_api) {
			g_api->Log("[UnitDamageCap] asmjit patch jit failed: %s",
				DebugUtils::errorAsString(err2));
		}
		return false;
	}
	if (code2.codeSize() > patchLen) {
		if (g_api) {
			g_api->Log("[UnitDamageCap] patch code too large: %zu > %zu",
				code2.codeSize(), patchLen);
		}
		rt2.release(fn2);
		return false;
	}
	std::memcpy(patchOut, fn2, code2.codeSize());
	rt2.release(fn2);

	// 回填补丁里的 jmp：这段字节将被写到 patchSiteAddr 处执行，
	// 所以 rel32 必须按 patchSiteAddr 计算，而不是按缓冲区地址。
	{
		const int64_t rel = reinterpret_cast<int64_t>(out) -
			(reinterpret_cast<int64_t>(patchSiteAddr) + 5);
		if (rel > INT32_MAX || rel < INT32_MIN) {
			if (g_api) {
				g_api->Log("[UnitDamageCap] patch jmp out of rel32 range: %p -> %p",
					patchSiteAddr, out);
			}
			return false;
		}
		const int32_t r32 = static_cast<int32_t>(rel);
		std::memcpy(patchOut + 1, &r32, 4);
	}

	return true;
}

#undef UC_EMIT

} // namespace

void UnitDamageCap_Init(HookAPI* api) {
	if (!api) return;
	g_api = api;

	g_getMaxDamage = gc::ResolveVerified<GetMaxDamageFn>(api, kHookImageGameCoreXP2,
		kTargetGetMaxDamage, kSigGetMaxDamage, sizeof(kSigGetMaxDamage),
		"UnitDamageCap/GetMaxDamage");
	if (!g_getMaxDamage) {
		api->Log("%s", "[UnitDamageCap] GetMaxDamage unresolved - damage cap left as-is");
		return;
	}

	void* setDamage = gc::ResolveVerified<void*>(api, kHookImageGameCoreXP2,
		kTargetSetDamage, kSigSetDamage, sizeof(kSigSetDamage), "UnitDamageCap/SetDamage");
	if (!setDamage) return;

	g_clampSite = static_cast<uint8_t*>(setDamage) + OFF_CLAMP_LOAD;
	if (std::memcmp(g_clampSite, kOrigClampLoad, sizeof(kOrigClampLoad)) != 0) {
		api->Log("%s", "[UnitDamageCap] unexpected bytes at clamp site - skipped");
		return;
	}

	// 可执行 trampoline 页
	void* mem = VirtualAlloc(nullptr, kTrampolineSize, MEM_COMMIT | MEM_RESERVE,
		PAGE_EXECUTE_READWRITE);
	if (!mem) {
		api->Log("%s", "[UnitDamageCap] VirtualAlloc failed");
		return;
	}
	g_trampoline = static_cast<uint8_t*>(mem);

	uint8_t* returnAddr = static_cast<uint8_t*>(setDamage) + OFF_CLAMP_CALL;
	size_t trampolineLen = 0;
	uint8_t patch[sizeof(kOrigClampLoad)] = {};
	if (!BuildTrampoline(returnAddr, g_getMaxDamage, g_trampoline, kTrampolineSize,
			&trampolineLen, patch, sizeof(patch), g_clampSite)) {
		api->Log("%s", "[UnitDamageCap] trampoline build failed");
		VirtualFree(g_trampoline, 0, MEM_RELEASE);
		g_trampoline = nullptr;
		return;
	}
	FlushInstructionCache(GetCurrentProcess(), g_trampoline, trampolineLen);

	// 补丁字节由 asmjit 生成（见 BuildTrampoline 尾部），长度与原指令一致。
	if (!api->Patch(g_clampSite, patch, sizeof(patch))) {
		api->Log("%s", "[UnitDamageCap] Patch failed");
		VirtualFree(g_trampoline, 0, MEM_RELEASE);
		g_trampoline = nullptr;
		return;
	}

	api->Log("%s", "[UnitDamageCap] patched SetDamage clamp bound -> Unit::GetMaxDamage()");
}

void UnitDamageCap_Shutdown() {
	// 什么都不做。
	//
	// 退出对局时 GameCore 插件会被整体卸载，模块内存由引擎回收，钩子/补丁由
	// Loader 处理，这里手动清理没有意义。而且**主动 VirtualFree trampoline 是
	// 有害的**：SetDamage 里那条 jmp 补丁字节仍然指向它，一旦释放后又被调用就是
	// 跳进已释放内存。按文档 §6.3，补丁本就"按生效后不还原"设计，别当卸载清理用。
}