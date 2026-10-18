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

// .uhook：插件 DLL 映像内的 RWX 节，存放 hook 的机器码（各 256 字节）。
// runtime 用 GetModuleHandleExW(FROM_ADDRESS) 校验 detour 回调地址必须归属于
// 已加载的插件 DLL；asmjit JIT 内存不属于任何模块会被拒
// （"InstallHook rejected: caller/callback is not an active plugin"）。
// 注意：MSVC 在同时给出 write 与 execute 时会忽略 write（C4330）并生成
// Read+Execute 节；运行期的可写性由 linker /SECTION:.uhook,RWE 保证（最终 RWX）。
#pragma section(".uhook", read, execute)
#pragma comment(linker, "/SECTION:.uhook,RWE")
__declspec(allocate(".uhook")) alignas(16) inline unsigned char g_hookCode[4][256];

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
// detour 安装辅助：asmjit 生成 hook 体 -> 拷入插件 DLL 的 .uhook(RWX) 节 ->
// InstallHook（runtime 校验 detour 地址必须归属于已加载的插件 DLL）。
// slot: 0-3，对应 g_hookCode[4][256]。
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
    size_t size = code.codeSize();
    if (size > sizeof(g_hookCode[slot])) {
        g_api->Log("[UnlockModelLimit] hook code too large for %s: %zu > %zu bytes",
            name, size, sizeof(g_hookCode[slot]));
        rt.release(func);
        return false;
    }
    memcpy(g_hookCode[slot], func, size);
    rt.release(func);

    if (!g_api->InstallHook((void*)hookAddr, (void*)g_hookCode[slot], (void**)origSlot)) {
        g_api->Log("[UnlockModelLimit] InstallHook failed for %s @ 0x%llX (detour 0x%p)",
            name, (unsigned long long)hookAddr, (void*)g_hookCode[slot]);
        return false;
    }
    g_api->Log("[UnlockModelLimit] hooked %s @ %p (detour %p)",
        name, (void*)hookAddr, (void*)g_hookCode[slot]);
    return true;
}
