// UnlockModelLimit_Common.h
// DX11 / DX12 两个分支共享的工具与状态。
//
// 为什么要独立出来：DX11 与 DX12 是两份完全不同的二进制（RVA、ResourceList 布局、
// 站点集合都不同），实现必须分开；但下面这些是纯粹的"基础设施"——
// 内存写入、指令解码、hook 安装、exe 映像解析——两边逐字相同。
//
// 全部用 inline（函数）与 inline 变量（C++17），这样放在头文件里也不会重复定义。
#pragma once

#include "pch.h"
#include "HookAPI.h"
#include "asmjit/asmjit.h"
#include "Zydis/Zydis.h"
#include <vector>
#include <cstring>
#include <cstdint>
#include <algorithm>

// ---------------------------------------------------------------------------
// 全局状态
// ---------------------------------------------------------------------------
inline HookAPI* g_api = nullptr;

// asmjit JIT runtime（持有 hook 可执行代码的临时副本）
inline asmjit::JitRuntime rt;

// .uhook：插件 DLL 映像内的机器码槽，存放 asmjit 生成的 detour 体（各 256 字节）。
//
// 【为什么必须放在模块内】runtime 用 GetModuleHandleExW(FROM_ADDRESS) 校验 detour
// 地址必须归属于已加载的插件 DLL；asmjit 的 JIT 页不属于任何模块，会被拒
// （"InstallHook rejected: caller/callback is not an active plugin"）。
//
// 【W^X】节声明为 read+write（【不可执行】），不再用 /SECTION:.uhook,RWE 造永久 RWX。
// 每个槽在 FinalizeAndInstall 里走一遍 "临时 RW -> 写入 -> 切回 RX -> 才 InstallHook"：
//   * 永久 RWX 的代价：任何一次越界写都能把数据直接变成可执行代码；写坏了也不会立刻
//     崩，而是延迟到某次调用跳进坏指令，那时栈早已与被写坏的那行无关。
//   * 顺序不能反：入口字节一旦被 InstallHook 改写，游戏线程随时可能跳进 detour，
//     此时槽所在页必须已经是 RX，否则执行违例。
//
// 【为什么每个槽独占一页】VirtualProtect 按页生效。若 4 个槽共用一页，安装第 2 个
// hook 时把页切回 RW，就会让【已经在执行】的第 1 个 detour 变成不可执行而崩溃。
// alignas(4096) 让每个槽各占一页，切换只影响自己。
#pragma section(".uhook", read, write)
__declspec(allocate(".uhook")) alignas(4096) inline unsigned char g_hookCode0[256];
__declspec(allocate(".uhook")) alignas(4096) inline unsigned char g_hookCode1[256];
__declspec(allocate(".uhook")) alignas(4096) inline unsigned char g_hookCode2[256];
__declspec(allocate(".uhook")) alignas(4096) inline unsigned char g_hookCode3[256];
inline unsigned char* const g_hookSlots[4] = {
    g_hookCode0, g_hookCode1, g_hookCode2, g_hookCode3
};
inline constexpr size_t kHookSlotBytes = 256;

// 各 hook 的 trampoline（InstallHook 的 original 槽位）
inline uintptr_t g_origAddResource    = 0;
inline uintptr_t g_origRelease        = 0;
inline uintptr_t g_origAddResourceSPI = 0;
inline uintptr_t g_origReleaseSPI     = 0;

// 记录所有 Patch 成功的地址，OnUnload 统一 Unpatch 恢复
inline std::vector<void*> g_patchedAddrs;

// 当前进程实际加载的 exe 映像（DX11 或 DX12），在 Init 里判定一次。
inline HookImageId g_exeImage = kHookImageInvalid;

// ---------------------------------------------------------------------------
// Zydis 薄封装：解码 addr 处第一条指令，输出长度与助记符（可选）。
// ---------------------------------------------------------------------------
inline bool DecodeInsn(uint64_t addr, size_t* outLen, const char** outMnemonic) {
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    ZydisDecodedInstruction insn;
    if (ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&decoder, nullptr, (const void*)addr, 16, &insn))) {
        if (outLen) *outLen = insn.length;
        if (outMnemonic) *outMnemonic = ZydisMnemonicGetString(insn.mnemonic);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 内存写入封装（替代 modifyBytesAtAddress）：经 HookAPI::Patch 写入并记录。
// ---------------------------------------------------------------------------
inline bool PatchBytes(uint64_t addr, const std::vector<uint8_t>& bytes) {
    if (!g_api->Patch((void*)addr, bytes.data(), bytes.size())) {
        g_api->Log("[UnlockModelLimit] Patch failed @ 0x%llX", (unsigned long long)addr);
        return false;
    }
    g_patchedAddrs.push_back((void*)addr);
    return true;
}

// 验证失败 rollback：Unpatch 恢复原字节并从记录里移除该地址。
inline void UnpatchAndForget(uint64_t addr) {
    if (g_api) g_api->Unpatch((void*)addr);
    auto it = std::find(g_patchedAddrs.begin(), g_patchedAddrs.end(), (void*)addr);
    if (it != g_patchedAddrs.end()) g_patchedAddrs.erase(it);
}

// ---------------------------------------------------------------------------
// exe 内地址解析（RVA -> 绝对地址）。返回 0 表示失败。
// DX11 与 DX12 的所有地址一律走这张映像，绝不硬编码。
// ---------------------------------------------------------------------------
inline uint64_t ResolveExe(uintptr_t rva, size_t span) {
    if (!g_api || g_exeImage == kHookImageInvalid) return 0;
    return (uint64_t)g_api->ResolveByRva(g_exeImage, rva, span);
}

// ---------------------------------------------------------------------------
// 站点解析（docs/PLUGIN_GUIDE.md §5.2）：HookInfo 主路径 + AOB 兜底 + 字节校验。
//
// 为什么不能只用 ResolveByHookInfo：它的 AOB 兜底只在主路径【返回空】时生效。
// 跨版本更新时 PdataMatch 可能把旧 RVA 矫正到函数入口附近 —— 地址可读，但
// 函数内偏移已经变了，要的那条指令不在那儿。这种"主路径成功但落点错误"的情况
// 不会触发 AOB 兜底，必须由调用方比对期望字节，不符就强制改走 AOB。
//
// 主路径与 AOB 都要过字节校验；两者都不符返回 0（fail-closed）。
// ---------------------------------------------------------------------------
inline uint64_t ResolveSite(const HookInfo& info, const uint8_t* expect, size_t n,
                            const char* name) {
    if (!g_api || g_exeImage == kHookImageInvalid) return 0;

    const uint64_t addr = (uint64_t)ResolveByHookInfo(g_api, g_exeImage, info, n);
    if (addr && (!expect || memcmp((const void*)addr, expect, n) == 0)) return addr;
    if (addr) {
        g_api->Log("[UnlockModelLimit] %s: RVA 落点字节不符 @ 0x%llX，改走 AOB",
            name, (unsigned long long)addr);
    }
    if (info.aob && *info.aob) {
        void* p = g_api->ResolveByPattern(g_exeImage, info.aob, kSectionText, info.index);
        if (p && (!expect || memcmp(p, expect, n) == 0)) {
            g_api->Log("[UnlockModelLimit] %s: AOB 命中 @ 0x%llX", name,
                (unsigned long long)(uint64_t)p);
            return (uint64_t)p;
        }
    }
    g_api->Log("[UnlockModelLimit] %s: 站点未定位（RVA + AOB 均失败）", name);
    return 0;
}

// ---------------------------------------------------------------------------
// asmjit emit 检查宏。
//
// 为什么必需：asmjit 1.17 的非法操作数（最典型 cmp r64, imm64 且 imm > int32）
// 只写进 emitter 的局部 err，CodeHolder / JitRuntime::_add 都不检查 ——
// rt.add() 照样返回 kErrorOk，而那条指令已经被【静默丢弃】。于是 hook 体少了一条
// 指令仍然被装上，行为错乱且没有任何日志。所以每一条 emit 都必须自己检查。
//
// 用法：ASMJIT_EMIT(a.cmp(x86::rbx, (uint64_t)x));
// ---------------------------------------------------------------------------
#define ASMJIT_EMIT(expr)                                                                  \
    do {                                                                                   \
        asmjit::Error _e = (expr);                                                          \
        if (_e != asmjit::kErrorOk) {                                                       \
            g_api->Log("[UnlockModelLimit] asmjit emit failed @%d (%s): %s",                \
                __LINE__, #expr, asmjit::DebugUtils::errorAsString(_e));                    \
            return false;                                                                   \
        }                                                                                   \
    } while (0)

// ---------------------------------------------------------------------------
// 把 asmjit 生成的 hook 体写进某个 .uhook 槽，并按 W^X 切页。
//
// 时间线：RW(节初始) -> 写入 -> RX -> 交给 InstallHook。
// 只在【本槽】这一页上切换：四个槽各占独立页，不会波及已经在执行的其它 detour。
// ---------------------------------------------------------------------------
inline bool WriteHookCode(int slot, const void* src, size_t size, const char* name) {
    if (slot < 0 || slot >= 4) {
        g_api->Log("[UnlockModelLimit] invalid hook slot %d (%s)", slot, name);
        return false;
    }
    if (size == 0 || size > kHookSlotBytes) {
        g_api->Log("[UnlockModelLimit] hook code size %zu out of range (0, %zu] for %s",
            size, kHookSlotBytes, name);
        return false;
    }
    unsigned char* dst = g_hookSlots[slot];
    DWORD oldProtect = 0;
    // 节是 read+write：写入本身不需要改权限。这里显式再探一次，顺带拿到原权限，
    // 万一被外部改过（例如调试器）也能恢复到我们期望的状态。
    if (!VirtualProtect(dst, kHookSlotBytes, PAGE_READWRITE, &oldProtect)) {
        g_api->Log("[UnlockModelLimit] VirtualProtect(RW) failed for %s err=%lu",
            name, GetLastError());
        return false;
    }
    memcpy(dst, src, size);
    // 剩余字节清零：避免上一次安装残留的指令尾巴被误当代码执行。
    if (size < kHookSlotBytes) {
        memset(dst + size, 0xCC, kHookSlotBytes - size);
    }
    // 切到 RX：此后不可写、可执行。必须在 InstallHook 之前完成。
    if (!VirtualProtect(dst, kHookSlotBytes, PAGE_EXECUTE_READ, &oldProtect)) {
        g_api->Log("[UnlockModelLimit] VirtualProtect(RX) failed for %s err=%lu",
            name, GetLastError());
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), dst, kHookSlotBytes);
    return true;
}

// ---------------------------------------------------------------------------
// detour 安装辅助：asmjit 生成 hook 体 -> 写进插件 DLL 的 .uhook 槽（W^X 切页）->
// InstallHook（runtime 校验 detour 地址必须归属于已加载的插件 DLL）。
// slot: 0-3，对应 g_hookSlots[4]。
// ---------------------------------------------------------------------------
inline bool FinalizeAndInstall(uint64_t hookAddr, uintptr_t* origSlot, const char* name,
    asmjit::CodeHolder& code, int slot) {
    using namespace asmjit;
    void* func = nullptr;
    Error err = rt.add(&func, &code);
    if (err != kErrorOk || !func) {
        g_api->Log("[UnlockModelLimit] asmjit jit failed for %s @ 0x%llX: %s",
            name, (unsigned long long)hookAddr, asmjit::DebugUtils::errorAsString(err));
        return false;
    }
    const size_t size = code.codeSize();
    const bool written = WriteHookCode(slot, func, size, name);
    rt.release(func);
    if (!written) return false;

    if (!g_api->InstallHook((void*)hookAddr, (void*)g_hookSlots[slot], (void**)origSlot)) {
        g_api->Log("[UnlockModelLimit] InstallHook failed for %s @ 0x%llX (detour 0x%p)",
            name, (unsigned long long)hookAddr, (void*)g_hookSlots[slot]);
        return false;
    }
    g_api->Log("[UnlockModelLimit] hooked %s @ %p (detour %p, %zu bytes)",
        name, (void*)hookAddr, (void*)g_hookSlots[slot], size);
    return true;
}
