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
// 后果：MOD 把单位 MaxHitPoints 提到 100 以上时，伤害永远到不了 MaxHitPoints，
// 单位打不死。ChangeDamage 只是 SetDamage(m_iDamage + n)，改 SetDamage 一处
// 即可覆盖所有加伤路径。
//
// ---------------------------------------------------------------------------
// 修法：在函数内部 0x13F3C5A87 装 detour，接管 Range 的上界
// ---------------------------------------------------------------------------
// InstallHook 的目标不必是函数入口 —— Global 的 UnlockModelLimit 就是这么用的
// （hook 点在 AddResource 函数中段，detour 末尾 xchg+ret 跳回）。本模块同理：
//
//   hook 点：0x13F3C5A87  mov r8d, [rax+1E8h]（取全局上限那一条）
//   detour ：.uhook 槽里的 asmjit 机器码
//   跳回点：0x13F3C5A8E  call Utilities::Range
//
// detour 里调用 GetMaxDamage(unit) 得到真实上限，写进 r8d，再跳回 Range 之前。
//
// 【为什么这样可以省掉手写远跳】
//   InstallHook 底层是 MinHook，它自己处理远距离跳转（FF 25 绝对跳），
//   不依赖 jmp rel32 的 ±2GB 范围。实测插件 DLL 与 GameCore 相距约 131000 GB，
//   手写 rel32 根本不可达（VirtualAlloc 拿到的地址距补丁点 1528 GB），
//   交给 MinHook 处理才是正解。
//
// 【为什么 detour 必须放在 .uhook 节】
//   Loader 用 GetModuleHandleExW(FROM_ADDRESS) 校验 detour 地址归属，
//   VirtualAlloc / JitRuntime 分配的页不属于任何模块，会被拒绝
//   （"InstallHook rejected: caller/callback is not an active plugin"）。
//
// ---------------------------------------------------------------------------
// 定位（已用 tools/rva_to_symbol.exe 核对）
// ---------------------------------------------------------------------------
//   SetDamage        RVA 0x3A5A60   GameCore::Unit::Instance::SetDamage
//   hook 点          RVA 0x3A5A87   SetDamage+0x27
//   跳回点           RVA 0x3A5A8E   SetDamage+0x2E，call Utilities::Range
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

// ---------------------------------------------------------------------------
// .uhook：插件 DLL 映像内的机器码槽（W^X，独占一页）
//
// 声明为 read+write（不可执行），写完切 RX —— 不留永久 RWX 页。
// alignas(4096) 保证独占一页：VirtualProtect 按页生效，共用页会让已在执行的
// 代码在别人切页时变成不可执行。
// ---------------------------------------------------------------------------
#pragma section(".uhook", read, write)
__declspec(allocate(".uhook")) alignas(4096) unsigned char g_detourCode[4096];

constexpr size_t kDetourCapacity = sizeof(g_detourCode);

constexpr HookInfo kTargetSetDamage = HookName(
	"GameCore::Unit::Instance::SetDamage",
	"48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 40 48 8B 05 ?? ?? ?? ?? 44 8B CA 8B B1 C0 04 00 00");
constexpr uint8_t kSigSetDamage[8] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74 };

constexpr HookInfo kTargetGetMaxDamage = HookName(
	"GameCore::Unit::Instance::GetMaxDamage",
	"48 83 EC 28 48 81 C1 F0 01 00 00 BA 11 C7 3F 7D");
constexpr uint8_t kSigGetMaxDamage[8] = { 0x48, 0x83, 0xEC, 0x28, 0x48, 0x81, 0xC1, 0xF0 };

// hook 点：SetDamage+0x27（0x3A5A87），被替换的是
//   44 8B 80 E8 01 00 00   mov r8d, [rax+1E8h]
constexpr size_t OFF_HOOK = 0x27;
constexpr uint8_t kExpectAtHook[7] = { 0x44, 0x8B, 0x80, 0xE8, 0x01, 0x00, 0x00 };
// 跳回点：SetDamage+0x2E（0x3A5A8E），call Utilities::Range
constexpr size_t OFF_RESUME = 0x2E;

typedef int(__fastcall* GetMaxDamageFn)(void* unit);

HookAPI* g_api = nullptr;
GetMaxDamageFn g_getMaxDamage = nullptr;
size_t g_detourLen = 0;

// 生成一条 asmjit 指令；失败时写日志并返回 false。
// asmjit 对非法操作数只写进 emitter 的局部 err，rt.add() 仍返回 kErrorOk，
// 那条指令会被静默丢弃 —— 必须逐条检查。
bool Emit(asmjit::x86::Assembler& a, asmjit::Error e, int line) {
	if (e == asmjit::kErrorOk) return true;
	if (g_api) {
		g_api->Log("[UnitDamageCap] asmjit emit failed @%d: %s",
			line, asmjit::DebugUtils::errorAsString(e));
	}
	return false;
}

#define UC_EMIT(expr) do { if (!Emit(a, (expr), __LINE__)) return 0; } while (0)

// 生成 detour 到 out，返回写入长度（0 = 失败）。
//
// 进入时寄存器（hook 点之前的三条指令已执行）：
//   rbx = unit          (0x13f3c5a7f mov rbx, rcx)
//   r9d = finalDamage   (0x13f3c5a76)
//   ecx = finalDamage   (0x13f3c5a84 mov ecx, r9d)
//   edx = 0             (0x13f3c5a82 xor edx, edx)
//   esi = old damage    (0x13f3c5a79)
//   rax = ms_pkParameters（原指令要用的，但之后不再被读，可覆盖）
//
// 我们只需要把 r8d 换成真实上限，然后跳回 call Utilities::Range。
// 栈对齐：MinHook 跳到 detour 时 rsp%16 == 8（call 压了返回地址），
// 要让 call GetMaxDamage 前 rsp%16 == 0，压栈总量须为 8 的奇数倍。
size_t BuildDetour(uint8_t* out, size_t cap, void* resumeAddr) {
	using namespace asmjit;

	JitRuntime rt;
	CodeHolder code;
	code.init(rt.environment());
	x86::Assembler a(&code);

	// 保存会被 GetMaxDamage 破坏、而跳回点仍需要的寄存器。
	// r8 不保存 —— 它的旧值（全局上限）本来就是我们要替换掉的，跳回点也只用新值。
	UC_EMIT(a.push(x86::rcx));
	UC_EMIT(a.push(x86::rdx));
	UC_EMIT(a.push(x86::r9));
	UC_EMIT(a.push(x86::rsi));
	UC_EMIT(a.push(x86::rdi));
	UC_EMIT(a.push(x86::rax));      // 6 个 push = 48；进入时 rsp%16=8 -> (8-48)%16=8
	UC_EMIT(a.sub(x86::rsp, 8));    // 再减 8 -> rsp%16=0，满足 call 前对齐

	UC_EMIT(a.mov(x86::rcx, x86::rbx));            // rcx = unit
	UC_EMIT(a.mov(x86::rax, (uint64_t)g_getMaxDamage));
	UC_EMIT(a.call(x86::rax));                     // eax = 真实上限
	UC_EMIT(a.mov(x86::r8d, x86::eax));            // r8d = 真实上限（Range 上界）

	UC_EMIT(a.add(x86::rsp, 8));
	UC_EMIT(a.pop(x86::rax));
	UC_EMIT(a.pop(x86::rdi));
	UC_EMIT(a.pop(x86::rsi));
	UC_EMIT(a.pop(x86::r9));
	UC_EMIT(a.pop(x86::rdx));
	UC_EMIT(a.pop(x86::rcx));

	// 跳回 call Utilities::Range
	UC_EMIT(a.mov(x86::rax, (uint64_t)(uintptr_t)resumeAddr));
	UC_EMIT(a.jmp(x86::rax));

	void* fn = nullptr;
	const Error err = rt.add(&fn, &code);
	if (err != kErrorOk || !fn) {
		if (g_api) {
			g_api->Log("[UnitDamageCap] asmjit jit failed: %s",
				DebugUtils::errorAsString(err));
		}
		return 0;
	}
	const size_t size = code.codeSize();
	if (size > cap) {
		if (g_api) g_api->Log("[UnitDamageCap] detour too large: %zu > %zu", size, cap);
		rt.release(fn);
		return 0;
	}
	std::memcpy(out, fn, size);
	rt.release(fn);
	return size;
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

	uint8_t* base = static_cast<uint8_t*>(setDamage);
	uint8_t* hookAddr = base + OFF_HOOK;
	if (std::memcmp(hookAddr, kExpectAtHook, sizeof(kExpectAtHook)) != 0) {
		api->Log("%s", "[UnitDamageCap] unexpected bytes at hook site - skipped");
		return;
	}

	// 生成 detour 到 .uhook 槽
	void* resume = base + OFF_RESUME;
	g_detourLen = BuildDetour(g_detourCode, kDetourCapacity, resume);
	if (g_detourLen == 0) {
		api->Log("%s", "[UnitDamageCap] detour build failed");
		return;
	}

	// W^X：写完切 RX。必须在 InstallHook 之前 —— 入口一旦被改写，
	// 游戏线程随时可能跳进来，那时代码页必须已可执行。
	DWORD oldProtect = 0;
	if (!VirtualProtect(g_detourCode, kDetourCapacity, PAGE_EXECUTE_READ, &oldProtect)) {
		api->Log("[UnitDamageCap] VirtualProtect(RX) failed: %lu", GetLastError());
		return;
	}
	FlushInstructionCache(GetCurrentProcess(), g_detourCode,
		static_cast<DWORD>(g_detourLen));

	// 原函数跳板：MinHook 把被替换的那条指令搬到这里，供 detour 调用。
	// 本 detour 不需要调用原指令（它只是取全局上限，我们已用自己的值替代），
	// 但仍需接收 original 指针以满足 InstallHook 的契约。
	void* original = nullptr;
	if (!api->InstallHook(hookAddr, g_detourCode, &original)) {
		api->Log("%s", "[UnitDamageCap] InstallHook failed");
		return;
	}

	api->Log("[UnitDamageCap] hooked SetDamage+0x27; Range upper bound now "
		"Unit::GetMaxDamage() (detour=%u bytes, orig=%p)",
		static_cast<unsigned>(g_detourLen), original);
}

void UnitDamageCap_Shutdown() {
	// 什么都不做。钩子由 Loader 按 owner 自动摘除；.uhook 槽随插件映像一起回收。
}