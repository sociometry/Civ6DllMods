// UnlockModelLimit.cpp
// 模型上限翻倍 —— 从 DllProxy 工程（PIC_ExternalFunctions.cpp）忠实移植，
// 只做接口层机械替换：
//   PLH::x64Detour          -> HookAPI::InstallHook
//   modifyBytesAtAddress    -> HookAPI::Patch / Unpatch
//   PLH::ZydisDisassembler  -> 本文件 DecodeInsn（Zydis 薄封装）
//   std::cout / MessageBox  -> HookAPI::Log
//   手工 exeBase+偏移        -> HookAPI::ResolveByRva（ResolveExe helper）
//
// 逻辑本体（SPI/IBVB 扩展数组分配、SPI 12 点 + IBVB 31 点重定向字节补丁、
// 4 个写点 detour 的 asmjit 汇编、固定字节补丁 7 处）逐条照抄参考实现。

#include "pch.h"
#include "HookAPI.h"
#include "UnlockModelLimit.h"
#include "UnlockModelLimit_Common.h"
#include "asmjit/asmjit.h"
#include "Zydis/Zydis.h"
#include <vector>
#include <cstring>
#include <cstdint>
#include <algorithm>

// 站点 AOB 表（可选生成物）：存在时给补丁站点启用 AOB 兜底。
#if __has_include("UnlockModelLimit_Dx11_Sites.h")
#include "UnlockModelLimit_Dx11_Sites.h"
#define DX11_HAS_SITE_AOB 1
#else
#define DX11_HAS_SITE_AOB 0
#endif

// ---------------------------------------------------------------------------
// 结构定义（照抄 DllProxy 工程的 PIC_Module.h；只用到 sizeof 与 0x28/0x2C 偏移）
// ---------------------------------------------------------------------------
// FGXShaderPackageInstance：m_cMutables(4) + m_iPackageID(4) + m_Mutables[4](8) = 16 字节
struct FGXShaderPackageInstance {
    uint32_t m_cMutables;
    uint32_t m_iPackageID;
    uint16_t m_Mutables[4];
};
static_assert(sizeof(FGXShaderPackageInstance) == 16, "FGXShaderPackageInstance must be 16 bytes");

// BufferExtraData：48 字节；iInputFormat @ 0x28，Format @ 0x2C
struct BufferExtraData {
    char Desc[24];
    bool bInstance;
    unsigned char _pad[7];
    void* pTempBuffer;
    unsigned int iInputFormat;
    unsigned char Format[4];
};
static_assert(sizeof(BufferExtraData) == 48, "BufferExtraData must be 48 bytes");

// ---- DX11 分支专属的全局（DX12 有自己的同名副本，见 UnlockModelLimit_Dx12.cpp）----
// IB/VB 原始上限 0x2000 = 8192（AddResource/Release detour 的 cmp 阈值）
alignas(64) static const int ORIG_LIMIT = 8192;
// SPI 原始上限 0x4000 = 16384（保留常量以便注释说明；固定字节补丁把它提到 0x8000）
alignas(64) static const int ORIG_LIMIT_SPI = 8192 * 2;

// ---- 列表基址：运行期解析 ----
// Loader 的 PdataMatch 只校正 .text 里的函数地址，.data 段的全局对象它查表返回 0，
// 写死 RVA 在跨版本时必然失效。改成从引用列表的 lea 指令里解 disp32。
inline uint64_t g_ibListVA = 0;
inline uint64_t g_vbListVA = 0;
inline uint64_t g_spiListVA = 0;
inline uint64_t g_computeSpiListVA = 0;

// 三处 AddRef 实例的指令序列逐字节相同（只有 disp32 不同）。
// 当前 exe 共 4 处命中，按地址升序：1 IB / 2 SPI / 3 Texture / 4 VB。
constexpr char kAob_AddRefList[] =
    "48 8D 0D xx xx xx xx | 48 8D 0C 81 8B 81 80 00 00 00 FF C0 89 81 80 00 00";
// CreateComputePackageInstance 的 lea 引用 m_ComputeSPIList。
constexpr char kAob_ComputeSPIList[] =
    "48 8D 2D xx xx xx xx | 41 8B F8 48 8B CD E8";

// SPI 扩展数组指针（below 2GB，见 InitSPIExtendedArrays）
static uint32_t* g_pSPIList_RefCount = nullptr;
static FGXShaderPackageInstance* g_pSPIList_Resources = nullptr;

// IB/VB 扩展数组指针（below 2GB，见 InitIBVBExtendedArrays）。
// ID3D11Buffer 在这里只作为指针数组的元素类型（8 字节/元素），用 void** 等价占位。
static uint32_t* g_pIB_RefCount = nullptr;
static void** g_pIB_Resources = nullptr;
static BufferExtraData* g_pIB_ExtraData = nullptr;
static uint32_t* g_pVB_RefCount = nullptr;
static void** g_pVB_Resources = nullptr;
static BufferExtraData* g_pVB_ExtraData = nullptr;




// ---------------------------------------------------------------------------
// SPI 扩展数组分配（照抄 DllProxy 行 185-210；std::cout -> Log，
// MessageBox/ExitProcess -> Log + return false）
// ---------------------------------------------------------------------------
// 列表基址解析：.data 绝对地址 Loader 校正不了，从引用指令里现取。
static bool ResolveListBases() {
    // 三处 AddRef（IB / SPI / VB）序列相同，按地址升序 + index 区分：
    // 1 m_IBList / 2 m_SPIList / 3 Texture / 4 m_VBList（已离线核对目标地址）。
    g_ibListVA = (uint64_t)ResolveDataByAob(g_api, g_exeImage, kAob_AddRefList, 1);
    g_spiListVA = (uint64_t)ResolveDataByAob(g_api, g_exeImage, kAob_AddRefList, 2);
    g_vbListVA = (uint64_t)ResolveDataByAob(g_api, g_exeImage, kAob_AddRefList, 4);
    g_computeSpiListVA = (uint64_t)ResolveDataByAob(g_api, g_exeImage, kAob_ComputeSPIList, 1);

    if (!g_ibListVA || !g_vbListVA || !g_spiListVA) {
        g_api->Log("UnlockModelLimit: 列表基址解析失败 IB=0x%llX VB=0x%llX SPI=0x%llX",
            (unsigned long long)g_ibListVA, (unsigned long long)g_vbListVA,
            (unsigned long long)g_spiListVA);
        return false;
    }
    if (g_ibListVA == g_vbListVA || g_ibListVA == g_spiListVA || g_vbListVA == g_spiListVA) {
        g_api->Log("UnlockModelLimit: 列表基址自检失败（地址重复）");
        return false;
    }
    g_api->Log("UnlockModelLimit: list bases IB=0x%llX VB=0x%llX SPI=0x%llX ComputeSPI=0x%llX",
        (unsigned long long)g_ibListVA, (unsigned long long)g_vbListVA,
        (unsigned long long)g_spiListVA, (unsigned long long)g_computeSpiListVA);
    return true;
}

static bool InitSPIExtendedArrays() {
    // m_cItems 是 u16（最大 65535），句柄空间 0x0000..0xFFFF，必须按 65536 项分配。
    // 【曾用 32768 导致崩溃】CreateShaderPackageInstance(0x946A37) 拿到的句柄 0xC000
    // (49152) 越界：R8 = base + 0xC000*0x10 = 0x4D40000，超出 0x4C80000+0x80000 的
    // Resources 区，落到未提交页 -> 0xC0000005 写 0x4D40004。
    // 与 IB/VB 是同一类错误（那边已按 65536 修好，这边漏了）。
    const size_t kItems    = 65536;                                     // 完整 16 位句柄空间
    const size_t kResBytes = kItems * sizeof(FGXShaderPackageInstance);  // 0x100000
    const size_t kRefBytes = kItems * sizeof(uint32_t);                  // 0x40000
    const size_t kTotal = kResBytes + kRefBytes;                        // 0x140000

    // 尽力分配在 2GB 以下（A/B 类补丁形式要求 sign-extended disp32/imm32 目标）
    for (int i = 0; i < 128; i++) {
        void* p = VirtualAlloc(NULL, kTotal, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!p) break;
        if ((uint64_t)p < 0x80000000ULL) {
            g_pSPIList_Resources = (FGXShaderPackageInstance*)p;
            g_pSPIList_RefCount = (uint32_t*)((uint8_t*)p + kResBytes);
            g_api->Log("[SPIPatch] extended arrays @ 0x%llX (ref 0x%llX)",
                (unsigned long long)(uint64_t)g_pSPIList_Resources,
                (unsigned long long)(uint64_t)g_pSPIList_RefCount);
            return true;
        }
        VirtualFree(p, 0, MEM_RELEASE);
    }

    g_api->Log("[UnlockModelLimit] Failed to allocate SPI extended arrays below 2GB");
    return false;
}

// ---------------------------------------------------------------------------
// IB/VB 扩展数组分配（照抄 DllProxy 行 145-176；同上错误处理替换）
// 每列表布局（65536 项 = 完整 16 位句柄空间）：RefCount[0x40000] + Resources[0x80000] + ExtraData[0x300000]
// ---------------------------------------------------------------------------
static bool InitIBVBExtendedArrays() {
    const size_t kItems    = 65536;                            // 完整 16 位句柄空间
    const size_t kRefBytes = kItems * sizeof(uint32_t);        // 0x40000
    const size_t kResBytes = kItems * sizeof(void*);           // 0x80000
    const size_t kExtBytes = kItems * sizeof(BufferExtraData); // 0x300000
    const size_t kPerList = kRefBytes + kResBytes + kExtBytes; // 0x3C0000
    const size_t kTotal = kPerList * 2;                        // 0x780000

    for (int i = 0; i < 128; i++) {
        void* p = VirtualAlloc(NULL, kTotal, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!p) break;
        if ((uint64_t)p < 0x80000000ULL) {
            uint8_t* base = (uint8_t*)p;
            g_pIB_RefCount = (uint32_t*)base;
            g_pIB_Resources = (void**)(base + kRefBytes);
            g_pIB_ExtraData = (BufferExtraData*)(base + kRefBytes + kResBytes);
            g_pVB_RefCount = (uint32_t*)(base + kPerList);
            g_pVB_Resources = (void**)(base + kPerList + kRefBytes);
            g_pVB_ExtraData = (BufferExtraData*)(base + kPerList + kRefBytes + kResBytes);
            g_api->Log("[IBVBPatch] extended arrays @ 0x%llX / 0x%llX",
                (unsigned long long)(uint64_t)g_pIB_Resources,
                (unsigned long long)(uint64_t)g_pVB_Resources);
            return true;
        }
        VirtualFree(p, 0, MEM_RELEASE);
    }

    g_api->Log("[UnlockModelLimit] Failed to allocate IB/VB extended arrays below 2GB");
    return false;
}

// ---------------------------------------------------------------------------
// EncodeAndApplySPIPatch（照抄 DllProxy 行 250-369）
//   isLea=true : mov dstReg, imm32        （A 类，7 字节：REX.W+C7/0）
//   isLea=false: mov dstReg.r32(), [idxReg*8+disp32]（B 类，7/8 字节，绝对寻址）
// 写入后经 Zydis 回读验证；验证失败用 Unpatch 回滚并移除记录。
// ---------------------------------------------------------------------------
static bool EncodeAndApplySPIPatch(uint64_t addr, uint64_t target, bool isLea,
    asmjit::x86::Gp dstReg, asmjit::x86::Gp idxReg) {
    using namespace asmjit;

    uint8_t orig[16];
    memcpy(orig, (void*)addr, 16);

    // 原始指令长度（Zydis 解码第一条指令；默认 7）
    size_t origLen = 7;
    DecodeInsn(addr, &origLen, nullptr);
    g_api->Log("[SPIPatch] orig len @ 0x%llX: %zu", (unsigned long long)addr, origLen);

    // Sanity check：补丁点当前必须是指定形式。REX 前缀放宽识别：
    // 实测当前 exe 里 B 类点前缀为 0x42/0x44/0x4C 等变体（不限于 0x41），
    // A 类点为 0x48/0x4C —— 统一用 (orig[0] & 0xF0) == 0x40 判定。
    int off = ((orig[0] & 0xF0) == 0x40) ? 1 : 0;
    if (isLea) {
        if (!(orig[off] == 0x8D && (orig[off + 1] & 0xC7) == 0x05)) {
            g_api->Log("[SPIPatch] unexpected lea bytes at 0x%llX", (unsigned long long)addr);
            return false;
        }
    } else {
        if (!(orig[off] == 0x8B && orig[off + 1] == 0x84)) {
            g_api->Log("[SPIPatch] unexpected mov bytes at 0x%llX", (unsigned long long)addr);
            return false;
        }
    }

    // 目标必须能表示为 sign-extended imm32（target < 2GB）
    int64_t sext = (int64_t)(int32_t)(uint32_t)target;
    if (sext != (int64_t)target) {
        g_api->Log("[SPIPatch] target not imm32-representable at 0x%llX: target=0x%llX (arrays must be below 2GB)",
            (unsigned long long)addr, (unsigned long long)target);
        return false;
    }
    int32_t disp = (int32_t)(uint32_t)target;

    CodeHolder code;
    code.init(rt.environment());
    x86::Assembler a(&code);
    if (isLea) {
        // mov r64, imm32 (sign-extended) - 7 字节
        ASMJIT_EMIT(a.mov(dstReg, (int64_t)target));
    } else {
        // mov r32, [idx*8 + disp32]：仅索引 SIB 形式，x64 下为绝对寻址
        x86::Mem m;
        m.setIndex(idxReg);
        m.setShift(3);
        m.setOffset(disp);
        ASMJIT_EMIT(a.mov(dstReg.r32(), m));
    }

    void* func = nullptr;
    Error err = rt.add(&func, &code);
    if (err != kErrorOk || !func) {
        g_api->Log("[SPIPatch] asmjit encode failed at 0x%llX: %s",
            (unsigned long long)addr, asmjit::DebugUtils::errorAsString(err));
        return false;
    }
    size_t size = code.codeSize();
    std::vector<uint8_t> nb((uint8_t*)func, (uint8_t*)func + size);
    rt.release(func);

    if (nb.size() > origLen) {
        g_api->Log("[SPIPatch] encoded length %zu exceeds original %zu at 0x%llX",
            nb.size(), origLen, (unsigned long long)addr);
        return false;
    }
    // NOP 填充覆盖原指令（避免指令流错位）
    while (nb.size() < origLen) {
        nb.push_back(0x90);
    }

    if (!PatchBytes(addr, nb)) {
        g_api->Log("[SPIPatch] write failed at 0x%llX", (unsigned long long)addr);
        return false;
    }

    // Zydis 回读验证
    size_t vLen = 0;
    const char* vMnemonic = nullptr;
    bool ok = false;
    if (DecodeInsn(addr, &vLen, &vMnemonic)) {
        ok = (vLen == 7) && vMnemonic &&
            (strcmp(vMnemonic, "mov") == 0 || strcmp(vMnemonic, "lea") == 0);
        if (ok) {
            uint8_t b[7];
            memcpy(b, (void*)addr, 7);
            if (isLea) {
                // mov r64, imm32：REX.W + C7 /0
                ok = ((b[0] & 0xF8) == 0x48) && (b[1] == 0xC7) && ((b[2] & 0xC0) == 0xC0);
            } else {
                // mov r32, [idx*8+disp32]：ModRM mod=00 rm=100(SIB)，SIB base=101
                int voff = (b[0] == 0x40) ? 1 : 0;   // 容忍不带 B 位的 REX 前缀
                ok = ((b[voff + 1] & 0xC0) == 0x00) && ((b[voff + 1] & 0x07) == 0x04)
                    && ((b[voff + 2] & 0x07) == 0x05);
            }
        }
        g_api->Log("[SPIPatch] verify 0x%llX len=%zu mn=%s ok=%d",
            (unsigned long long)addr, vLen, vMnemonic ? vMnemonic : "<?>", ok ? 1 : 0);
    }
    if (!ok) {
        g_api->Log("[SPIPatch] Zydis verify FAILED at 0x%llX - rolling back", (unsigned long long)addr);
        UnpatchAndForget(addr);
        return false;
    }

    g_api->Log("[SPIPatch] %s 0x%llX -> 0x%llX", isLea ? "mov-imm" : "mov-idx",
        (unsigned long long)addr, (unsigned long long)target);
    return true;
}

// RVA 解析失败时的 AOB 兜底（同 DX12 分支）。
static uint64_t SiteFallback(uintptr_t rva) {
#if DX11_HAS_SITE_AOB
    for (const auto& site : kDx11SiteAobs) {
        if (site.rva != rva) continue;
        void* hit = g_api->ResolveByPattern(g_exeImage, site.aob, kSectionText, 1);
        if (!hit) return 0;
        g_api->Log("UnlockModelLimit: RVA 0x%X 失效，AOB 定位到 0x%llX",
            (unsigned)rva, (unsigned long long)(uint64_t)hit);
        return (uint64_t)hit + site.delta;
    }
#endif
    return 0;
}

// SPI 补丁点包装：先用 ResolveExe 解析 RVA；解析失败 Log 并跳过该点。
static void ApplySPIPatchSite(uint64_t rva, uint64_t target, bool isLea,
    asmjit::x86::Gp dstReg, asmjit::x86::Gp idxReg) {
    uint64_t addr = ResolveExe(rva, 16);
    if (!addr) addr = SiteFallback(rva);
    if (!addr) {
        g_api->Log("UnlockModelLimit: RVA 0x%X unavailable", (unsigned int)rva);
        return;
    }
    EncodeAndApplySPIPatch(addr, target, isLea, dstReg, idxReg);
}

// ---------------------------------------------------------------------------
// SPI 重定向补丁（照抄 DllProxy 行 385-419）：mask=0xFFF 全 12 点。
// 地址经 ResolveExe（ResolveByRva）解析。
// ---------------------------------------------------------------------------
static void ApplySPIRedirectPatches() {
    // Bisect switch：0xFFF = 全部 12 个静态引用
    int mask = 0xFFF;
    g_api->Log("[SPIPatch] mask=0x%X", mask);

    const uint64_t res = (uint64_t)g_pSPIList_Resources;   // FGXShaderPackageInstance[65536]
    const uint64_t ref = (uint64_t)g_pSPIList_RefCount;    // uint32_t[65536]

    using namespace asmjit;

    // A 类 mov-imm -> g_SPIList_Resources（或 +4 为 m_iPackageID / RefCount 变体）
    if (mask & 0x001) ApplySPIPatchSite(0x946A37, res, true, x86::rax, x86::rax);      // CreateShaderPackageInstance
    if (mask & 0x002) ApplySPIPatchSite(0x957990, res, true, x86::rax, x86::rax);      // RenderSurface
    if (mask & 0x004) ApplySPIPatchSite(0x956B15, res, true, x86::rax, x86::rax);      // ReleaseShaderPackageInstance
    if (mask & 0x008) ApplySPIPatchSite(0x95D775, res, true, x86::rcx, x86::rcx);      // Shutdown memset pDst
    if (mask & 0x010) ApplySPIPatchSite(0x94D013, res + 4, true, x86::rcx, x86::rcx);  // GetEnumFromShaderPackageInstance
    if (mask & 0x020) ApplySPIPatchSite(0x95D783, ref, true, x86::rcx, x86::rcx);      // Shutdown memset pDst
    if (mask & 0x040) ApplySPIPatchSite(0x95CEB5, ref, true, x86::r13, x86::r13);      // Shutdown refcount clear loop (r13)
    if (mask & 0x080) ApplySPIPatchSite(0x940542, ref - 0x80, true, x86::rcx, x86::rcx); // AddRefShaderPackageInstance fake base

    // B 类 mov -> g_SPIList_Resources.m_iPackageID（绝对 [idx*8+disp32]）
    if (mask & 0x100) ApplySPIPatchSite(0x94ABAC, res + 4, false, x86::eax, x86::rax);  // ExecuteCommandSubset
    if (mask & 0x200) ApplySPIPatchSite(0x9504FB, res + 4, false, x86::eax, x86::rax);  // sub_140950400
    if (mask & 0x400) ApplySPIPatchSite(0x9506F9, res + 4, false, x86::eax, x86::rcx);  // sub_140950640 loop1
    if (mask & 0x800) ApplySPIPatchSite(0x950757, res + 4, false, x86::eax, x86::rax);  // sub_140950640 loop2
}

// ---------------------------------------------------------------------------
// EncodeAndApplyIBVBPatch（照抄 DllProxy 行 440-511）
// 注意：参考实现内部 addr += exeBase（调用方传 RVA）；移植版调用方经
// ResolveExe 传绝对地址，函数内部不再 +=。
//   kind 0 (A 类)：lea r64,[rip+disp32] -> mov r64, imm32（7 字节）
//   kind 1 (B 读)：mov r64/r32, [idx*2^shift+disp32]（8 字节，绝对）
//   kind 2 (B 写)：mov [idx*2^shift+disp32], r64/r32（8 字节，绝对）
//   kind 3 (lea-disp)：仅改写 disp32（CreateVBAtHandle；后续 add r9,rsi 另有 NOP）
// ---------------------------------------------------------------------------
static void EncodeAndApplyIBVBPatch(uint64_t addr, uint64_t target, int kind,
    asmjit::x86::Gp reg, asmjit::x86::Gp idx, int shift, bool is64) {
    using namespace asmjit;
    const size_t origLen = (kind == 0) ? 7 : 8;

    const char* beforeText = "<unknown>";
    size_t beforeLen = 0;
    DecodeInsn(addr, &beforeLen, &beforeText);

    if (kind == 3) {
        // lea idx*2^shift + disp32（绝对，无 base）：只改写 disp32
        uint32_t disp = (uint32_t)target;
        std::vector<uint8_t> nb = {
            (uint8_t)(disp & 0xFF), (uint8_t)((disp >> 8) & 0xFF),
            (uint8_t)((disp >> 16) & 0xFF), (uint8_t)((disp >> 24) & 0xFF)
        };
        if (!PatchBytes(addr + 4, nb)) {
            g_api->Log("[IBVBPatch] lea-disp write failed at 0x%llX", (unsigned long long)addr);
            return;
        }
        size_t vLen = 0;
        bool ok = DecodeInsn(addr, &vLen, nullptr) && vLen == 8;
        g_api->Log("[IBVBPatch] lea-disp 0x%llX -> 0x%llX ok=%d",
            (unsigned long long)addr, (unsigned long long)target, ok ? 1 : 0);
        g_api->Log("[IBVBPatch]    before: %s", beforeText ? beforeText : "<unknown>");
        return;
    }

    CodeHolder code;
    code.init(rt.environment());
    x86::Assembler a(&code);
    // 本函数返回 void，不能套 ASMJIT_EMIT（它在失败时 return false）——
    // 逐条检查并就地记录/返回。
    Error emitErr = kErrorOk;
    if (kind == 0) {
        emitErr = a.mov(reg, (int64_t)target);
    } else {
        x86::Mem m;
        m.setIndex(idx);
        m.setShift(shift);
        m.setOffset((int32_t)(uint32_t)target);
        if (kind == 1) {
            emitErr = is64 ? a.mov(reg, m) : a.mov(reg.r32(), m);
        } else {
            emitErr = is64 ? a.mov(m, reg) : a.mov(m, reg.r32());
        }
    }
    if (emitErr != kErrorOk) {
        // asmjit 1.17 的 emit 失败不会传播到 rt.add()，必须在这里拦下，
        // 否则残缺字节会被写进 exe。
        g_api->Log("[IBVBPatch] asmjit emit failed at 0x%llX (kind=%d): %s",
            (unsigned long long)addr, kind, asmjit::DebugUtils::errorAsString(emitErr));
        return;
    }

    void* func = nullptr;
    Error err = rt.add(&func, &code);
    if (err != kErrorOk || !func) {
        g_api->Log("[IBVBPatch] encode failed at 0x%llX: %s",
            (unsigned long long)addr, asmjit::DebugUtils::errorAsString(err));
        return;
    }
    size_t size = code.codeSize();
    std::vector<uint8_t> nb((uint8_t*)func, (uint8_t*)func + size);
    rt.release(func);
    // NOP 填充：低编号寄存器的 32 位 mov 只编 7 字节（无 REX）。
    while (nb.size() < origLen) {
        nb.push_back(0x90);
    }

    if (!PatchBytes(addr, nb)) {
        g_api->Log("[IBVBPatch] write failed at 0x%llX", (unsigned long long)addr);
        return;
    }

    size_t vLen = 0;
    const char* afterText = "<unknown>";
    // 32 位 mov 编成 7 字节并 NOP 填充；64 位正好 8 字节
    bool ok = DecodeInsn(addr, &vLen, &afterText) && (vLen == 7 || vLen == 8);
    g_api->Log("[IBVBPatch] kind=%d 0x%llX -> 0x%llX ok=%d",
        kind, (unsigned long long)addr, (unsigned long long)target, ok ? 1 : 0);
    g_api->Log("[IBVBPatch]    before: %s", beforeText ? beforeText : "<unknown>");
    g_api->Log("[IBVBPatch]    after:  %s", afterText ? afterText : "<unknown>");
}

// IBVB 补丁点包装：先用 ResolveExe 解析 RVA；解析失败 Log 并跳过该点。
static void ApplyIBVBPatchSite(uint64_t rva, uint64_t target, int kind,
    asmjit::x86::Gp reg, asmjit::x86::Gp idx, int shift, bool is64) {
    uint64_t addr = ResolveExe(rva, 16);
    if (!addr) addr = SiteFallback(rva);
    if (!addr) {
        g_api->Log("UnlockModelLimit: RVA 0x%X unavailable", (unsigned int)rva);
        return;
    }
    EncodeAndApplyIBVBPatch(addr, target, kind, reg, idx, shift, is64);
}

// ---------------------------------------------------------------------------
// IB/VB 重定向补丁（照抄 DllProxy 行 513-567）：
// AddRefIB/VB 复用 SPI 编码；其余 11 A 类 + 15 B 读 + 2 B 写 + 1 lea-disp
// + 1 NOP。地址经 ResolveExe（ResolveByRva）解析。
//
// 【二分拆分】原为一个函数，现按语义拆成两组，便于独立开关：
//   基址改写 = A 类 lea->mov + AddRefIB/VB + CreateVBAtHandle lea-disp/NOP
//   索引访问 = B 类 mov r,[idx*..] 读 + mov [idx*..],r 写
// 崩溃点 Layout_UploadBuffers 用的是「索引访问」读出的描述符，优先怀疑后者。
// ---------------------------------------------------------------------------
static void ApplyIBVBBasePatches() {
    using namespace asmjit;
    const uint64_t ibRes = (uint64_t)g_pIB_Resources;
    const uint64_t vbRes = (uint64_t)g_pVB_Resources;
    const uint64_t ibExt = (uint64_t)g_pIB_ExtraData;
    const uint64_t vbExt = (uint64_t)g_pVB_ExtraData;
    const uint64_t ibRef = (uint64_t)g_pIB_RefCount;
    const uint64_t vbRef = (uint64_t)g_pVB_RefCount;

    // AddRefIB/AddRefVB：lea rcx, m_IBList -> mov rcx, RefCount - 0x80
    ApplySPIPatchSite(0x9404F2, ibRef - 0x80, true, x86::rcx, x86::rax);
    ApplySPIPatchSite(0x9405E2, vbRef - 0x80, true, x86::rcx, x86::rax);

    // --- A 类：lea [rip+disp] -> mov r64, imm32 -------------------------
    ApplyIBVBPatchSite(0x94529B, ibRes, 0, x86::rdx, x86::rdx, 3, true);   // CreateIBAtHandle Resources base
    ApplyIBVBPatchSite(0x94508D, ibExt, 0, x86::rax, x86::rax, 4, true);   // CreateIBAtHandle ExtraData base
    ApplyIBVBPatchSite(0x947414, vbExt, 0, x86::rax, x86::rax, 4, true);   // CreateTBufferFromVB ExtraData base
    ApplyIBVBPatchSite(0x94749F, vbRes, 0, x86::r13, x86::r13, 3, true);   // CreateTBufferFromVB Resources base
    ApplyIBVBPatchSite(0x948D60, vbExt, 0, x86::rax, x86::rax, 4, true);   // CreateVBAtHandle ExtraData base
    ApplyIBVBPatchSite(0x95F866, ibExt, 0, x86::rcx, x86::rcx, 4, true);   // UnlockIB ExtraData base
    ApplyIBVBPatchSite(0x95FB86, vbExt, 0, x86::rcx, x86::rcx, 4, true);   // UnlockVB ExtraData base
    ApplyIBVBPatchSite(0x951945, ibRes, 0, x86::rcx, x86::rcx, 3, true);   // LockDynamicIB base
    ApplyIBVBPatchSite(0x951B85, vbRes, 0, x86::rcx, x86::rcx, 3, true);   // LockDynamicVB base
    ApplyIBVBPatchSite(0x95CD05, vbRef, 0, x86::r13, x86::r13, 2, true);   // Shutdown VB RefCount base
    ApplyIBVBPatchSite(0x95CD96, ibRef, 0, x86::r13, x86::r13, 2, true);   // Shutdown IB RefCount base

    // --- CreateVBAtHandle：绝对 lea + ImageBase add ----------------------
    ApplyIBVBPatchSite(0x948F3E, vbRes, 3, x86::r9, x86::r11, 3, true);    // lea r9, [r11*8+disp] -> disp=vbRes
    // add r9, rsi（rsi=0x140000000）会破坏绝对地址：NOP 掉
    uint64_t nopAddr = ResolveExe(0x948F4A, 4);
    if (!nopAddr) nopAddr = SiteFallback(0x948F4A);
    if (!nopAddr) {
        g_api->Log("UnlockModelLimit: RVA 0x%X unavailable", 0x948F4A);
    } else {
        const std::vector<uint8_t> nop3 = { 0x90, 0x90, 0x90 };
        if (!PatchBytes(nopAddr, nop3)) {
            g_api->Log("[IBVBPatch] nop add r9,rsi failed @ 0x948F4A");
        }
    }
    g_api->Log("[IBVBPatch] nop add r9,rsi @ 0x948F4A");
}

static void ApplyIBVBIndexedPatches() {
    using namespace asmjit;
    const uint64_t ibRes = (uint64_t)g_pIB_Resources;
    const uint64_t vbRes = (uint64_t)g_pVB_Resources;
    const uint64_t ibExt = (uint64_t)g_pIB_ExtraData;
    const uint64_t vbExt = (uint64_t)g_pVB_ExtraData;

    // --- B 读：mov r, [idx*2^shift + disp32] -----------------------------
    ApplyIBVBPatchSite(0x95C177, ibRes, 1, x86::rdx, x86::rdx, 3, true);   // SetVbIb IB Resources
    ApplyIBVBPatchSite(0x95C1B9, vbRes, 1, x86::rax, x86::rcx, 3, true);   // SetVbIb VB Resources
    ApplyIBVBPatchSite(0x95F88D, ibRes, 1, x86::rdx, x86::rax, 3, true);   // UnlockIB Resources
    ApplyIBVBPatchSite(0x95F973, ibRes, 1, x86::rdx, x86::rax, 3, true);   // UnlockIB Resources (dynamic)
    ApplyIBVBPatchSite(0x95FBAD, vbRes, 1, x86::rdx, x86::rax, 3, true);   // UnlockVB Resources
    ApplyIBVBPatchSite(0x95FC93, vbRes, 1, x86::rdx, x86::rax, 3, true);   // UnlockVB Resources (dynamic)
    ApplyIBVBPatchSite(0x9517CB, ibExt, 1, x86::r9d, x86::rax, 3, false);  // UpdateIBSubresource ExtraData (rax=idx*6)
    ApplyIBVBPatchSite(0x9517D5, ibRes, 1, x86::r10, x86::r8, 3, true);    // UpdateIBSubresource Resources
    ApplyIBVBPatchSite(0x95185B, vbExt, 1, x86::r9d, x86::rax, 3, false);  // UpdateVBSubresource ExtraData
    ApplyIBVBPatchSite(0x951865, vbRes, 1, x86::r10, x86::r8, 3, true);    // UpdateVBSubresource Resources
    ApplyIBVBPatchSite(0x954931, vbRes, 1, x86::rcx, x86::rcx, 3, true);   // Reap VB Resources
    ApplyIBVBPatchSite(0x954A31, ibRes, 1, x86::rcx, x86::rcx, 3, true);   // Reap IB Resources
    ApplyIBVBPatchSite(0x957F8A, vbExt + 0x28, 1, x86::edx, x86::rcx, 3, false); // RenderSurface ExtraData.iInputFormat (rcx=idx*6)
    ApplyIBVBPatchSite(0x95C183, ibExt + 0x2C, 1, x86::r8d, x86::rax, 0, false); // SetVbIb IB ExtraData.Format (rax=idx*48)
    ApplyIBVBPatchSite(0x95C1E8, vbExt + 0x28, 1, x86::eax, x86::rax, 3, false); // SetVbIb VB ExtraData.iInputFormat (rax=idx*6)

    // --- B 写：mov [idx*2^shift + disp32], r ----------------------------
    ApplyIBVBPatchSite(0x95494F, vbRes, 2, x86::r15, x86::rcx, 3, true);   // Reap VB Resources = null
    ApplyIBVBPatchSite(0x954A4F, ibRes, 2, x86::r15, x86::rcx, 3, true);   // Reap IB Resources = null
}


// ---------------------------------------------------------------------------
// 4 个 detour（替代 ValueReplace + x64Detour；asmjit 汇编逐条照抄参考
// 行 657-696 / 759-810）。所有 exe 内地址（hook 目标、数据立即数、跳回点）
// 均先经 ResolveExe（ResolveByRva）解析为局部 uint64_t 再嵌入汇编；
// 扩展数组指针（g_pSPIList_RefCount / g_pIB_RefCount / g_pVB_RefCount 等）
// 保持 (uint64_t) 直接强转。
// ---------------------------------------------------------------------------

// SPI AddResource @ RVA 0x940F31（indexReg=rcx, tempReg=r9）
static bool InstallHook_AddResourceSPI() {
    // 站点解析：RVA + AOB 兜底 + 字节校验（docs/PLUGIN_GUIDE.md §5.2）。
    // 注意 index=2：SPI 与 IB/VB 两处 AddResource 的指令序列逐字节相同，
    // 按地址升序 IB/VB(0x9408A1) 在前，SPI(0x940F31) 在后。
    static constexpr HookInfo kSite = HookRva(
        0x940F31, "FF 84 8B 80 00 00 00 89 73 40 49 8B 5B 10 49 8B 73 18 49 8B E3 5F", 2);
    static const uint8_t kExpect[] = { 0xFF, 0x84, 0x8B, 0x80, 0x00, 0x00, 0x00 };
    const uint64_t hookAddr = ResolveSite(kSite, kExpect, sizeof(kExpect), "AddResourceSPI");
    if (!hookAddr) {
        return false;
    }
    const uint64_t computeSPIList = g_computeSpiListVA;      // m_ComputeSPIList
    const uint64_t computeSPIRefCount = (g_computeSpiListVA + 0x80);  // m_ComputeSPIList.m_RefCount
    const uint64_t jumpBack = hookAddr + 0xE;                      // = inc 之后的收尾序列

    using namespace asmjit;
    CodeHolder code;
    code.init(rt.environment());
    x86::Assembler a(&code);
    x86::Gp tempReg = x86::r9;
    ASMJIT_EMIT(a.push(tempReg));
    ASMJIT_EMIT(a.push(x86::r10));
    ASMJIT_EMIT(a.mov(tempReg, (uint64_t)g_pSPIList_RefCount));  // 默认：m_SPIList 扩展数组
    ASMJIT_EMIT(a.mov(x86::r10, (uint64_t)computeSPIList));       // m_ComputeSPIList
    ASMJIT_EMIT(a.cmp(x86::rbx, x86::r10));                       // this == m_ComputeSPIList -> ZF=1
    ASMJIT_EMIT(a.mov(x86::r10, (uint64_t)computeSPIRefCount));   // m_ComputeSPIList.m_RefCount（原始）
    ASMJIT_EMIT(a.cmove(tempReg, x86::r10));                      // ZF=1 -> tempReg = 原始 refcount
    ASMJIT_EMIT(a.inc(x86::dword_ptr(tempReg, x86::rcx, 2)));
    ASMJIT_EMIT(a.pop(x86::r10));
    // 原指令 0x940F38 / 0x940F3B 在这里手工重放，然后跳 0x940F3F。
    // 第二条 mov rbx,[r11+0x10] 读 r11（原函数在 0x940F23 用 lea r11,[rsp+0x80]
    // 设好的帧指针）：hub stub 的快路径只碰 r10、慢路径在 ret 前复原 r11，
    // 所以 detour 入口处 r11 与调用方逐位相同，这里的重放成立。
    ASMJIT_EMIT(a.mov(x86::ptr(x86::rbx, 0x40), x86::esi));
    ASMJIT_EMIT(a.mov(x86::rbx, x86::ptr(x86::r11, 0x10)));

    // 注意：这里只 pop 了 r10，r9 仍留在栈上——这是参考实现（DllProxy）的既有行为：
    // 跳回点 0x940F3F 是函数尾声的收尾序列，会用 add rsp 自行收栈，不会读到我们压的 r9。
    // 不要"顺手补一个 pop"，那会破坏与跳回点的栈约定。
    ASMJIT_EMIT(a.mov(tempReg, (uint64_t)jumpBack));
    ASMJIT_EMIT(a.xchg(tempReg, x86::ptr(x86::rsp)));
    ASMJIT_EMIT(a.ret());

    return FinalizeAndInstall(hookAddr, &g_origAddResourceSPI, "AddResourceSPI", code, 0);
}

// SPI ReleaseResource @ RVA 0x956A51（indexReg=r8, tempReg=r9）
static bool InstallHook_ReleaseSPI() {
    // 站点解析：RVA + AOB 兜底 + 字节校验（docs/PLUGIN_GUIDE.md §5.2）。
    // index=2：IB/VB Release(0x956641) 与 SPI Release(0x956A51) 的 add+mov 序列相同。
    static constexpr HookInfo kSite = HookRva(
        0x956A51, "42 83 84 83 80 00 00 00 FF 42 8B B4 83 80 00 00 00 75 5F", 2);
    static const uint8_t kExpect[] = { 0x42, 0x83, 0x84, 0x83, 0x80, 0x00, 0x00, 0x00, 0xFF };
    const uint64_t hookAddr = ResolveSite(kSite, kExpect, sizeof(kExpect), "ReleaseSPI");
    if (!hookAddr) {
        return false;
    }
    const uint64_t computeSPIList = g_computeSpiListVA;      // m_ComputeSPIList
    const uint64_t computeSPIRefCount = (g_computeSpiListVA + 0x80);  // m_ComputeSPIList.m_RefCount
    const uint64_t jumpBack = hookAddr + 0x11;                     // = add+mov 之后

    using namespace asmjit;
    CodeHolder code;
    code.init(rt.environment());
    x86::Assembler a(&code);
    x86::Gp tempReg = x86::r9;
    ASMJIT_EMIT(a.push(tempReg));
    ASMJIT_EMIT(a.push(x86::r10));
    ASMJIT_EMIT(a.mov(tempReg, (uint64_t)g_pSPIList_RefCount));  // 默认：m_SPIList 扩展数组
    ASMJIT_EMIT(a.mov(x86::r10, (uint64_t)computeSPIList));       // m_ComputeSPIList
    ASMJIT_EMIT(a.cmp(x86::rbx, x86::r10));                       // this == m_ComputeSPIList -> ZF=1
    ASMJIT_EMIT(a.mov(x86::r10, (uint64_t)computeSPIRefCount));   // m_ComputeSPIList.m_RefCount（原始）
    ASMJIT_EMIT(a.cmove(tempReg, x86::r10));                      // ZF=1 -> tempReg = 原始 refcount
    ASMJIT_EMIT(a.add(x86::dword_ptr(tempReg, x86::r8, 2), -1));
    ASMJIT_EMIT(a.mov(x86::esi, x86::ptr(tempReg, x86::r8, 2)));
    ASMJIT_EMIT(a.pop(x86::r10));
    ASMJIT_EMIT(a.mov(tempReg, (uint64_t)jumpBack));
    ASMJIT_EMIT(a.xchg(tempReg, x86::ptr(x86::rsp)));
    ASMJIT_EMIT(a.ret());

    return FinalizeAndInstall(hookAddr, &g_origReleaseSPI, "ReleaseSPI", code, 1);
}

// IB/VB AddResource @ RVA 0x9408A1（indexReg=rcx, tempReg=r9；需要 firstSize）
static bool InstallHook_AddResource() {
    // 站点解析：RVA + AOB 兜底 + 字节校验（docs/PLUGIN_GUIDE.md §5.2）。
    // index=1：与 SPI AddResource 的指令序列逐字节相同，取地址较小的 IB/VB 那处。
    static constexpr HookInfo kSite = HookRva(
        0x9408A1, "FF 84 8B 80 00 00 00 89 73 40 49 8B 5B 10 49 8B 73 18 49 8B E3 5F", 1);
    static const uint8_t kExpect[] = { 0xFF, 0x84, 0x8B, 0x80, 0x00, 0x00, 0x00 };
    const uint64_t hookAddr = ResolveSite(kSite, kExpect, sizeof(kExpect), "AddResource");
    if (!hookAddr) {
        return false;
    }
    // g_ibListVA = m_IBList（0x142B0A740）。命中则走 label2 用 g_pIB_RefCount，
    // 否则（rbx == m_VBList）用 g_pVB_RefCount。变量名原为 mVBList，属误称。
    const uint64_t mIBList = g_ibListVA;

    using namespace asmjit;
    CodeHolder code;
    code.init(rt.environment());
    x86::Assembler a(&code);
    x86::Gp indexReg = x86::rcx;
    x86::Gp tempReg = x86::r9;
    x86::Gp listReg = x86::r10;   // 专用于 m_IBList 比较（规避 asmjit cmp r64,imm64 丢指令）
    int firstSize = 1;
    {
        size_t len = 1;
        if (DecodeInsn(hookAddr, &len, nullptr)) firstSize = (int)len;
    }
    Label label2 = a.newLabel();

    ASMJIT_EMIT(a.push(tempReg));
    ASMJIT_EMIT(a.push(listReg));   // 本 hook 是"跳回 + ret"，不是真调用：入口 push 必须自己配平

    // 【已移除 index 分流】所有句柄（含 < 8192 的低句柄）一律写扩展数组，
    // 不再跳 trampoline 开头去写原始数组。

    // 【不要写 a.cmp(x86::rbx, (uint64_t)mIBList)】——asmjit 1.17 对 cmp r64,imm64
    // 的校验是 if (size == 8) { if (!is_int_n<32>(imm)) goto InvalidImmediate; }，
    // 而 InvalidImmediate 只写进 emitter 的局部 err，CodeHolder / JitRuntime::_add 都不检查：
    // add() 照样返回 kErrorOk，却把整条 cmp 静默丢掉，je 于是沿用前面指令的标志位，
    // IB 路径可能永不可达。必须拆成 movabs + cmp reg,reg（下面的 ASMJIT_EMIT 是第二道保险：
    // 即便将来有人写回 imm64 形式，也会在安装前报错而不是静默丢指令）。
    ASMJIT_EMIT(a.mov(listReg, (uint64_t)mIBList));
    ASMJIT_EMIT(a.cmp(x86::rbx, listReg));
    ASMJIT_EMIT(a.je(label2));
    ASMJIT_EMIT(a.mov(tempReg, (uint64_t)g_pVB_RefCount));
    ASMJIT_EMIT(a.inc(x86::dword_ptr(tempReg, indexReg, 2)));
    ASMJIT_EMIT(a.mov(tempReg, (uint64_t)&g_origAddResource));
    ASMJIT_EMIT(a.mov(tempReg, x86::ptr(tempReg)));
    // add 跳过 trampoline 首条指令（原 inc）——不走原始数组，计数只落在扩展数组。
    ASMJIT_EMIT(a.add(tempReg, firstSize));
    // 顺序不可交换：必须先 pop 恢复 listReg，再 xchg 把跳回地址放到栈顶。
    ASMJIT_EMIT(a.pop(listReg));
    ASMJIT_EMIT(a.xchg(tempReg, x86::ptr(x86::rsp)));
    ASMJIT_EMIT(a.ret());

    a.bind(label2);
    ASMJIT_EMIT(a.mov(tempReg, (uint64_t)g_pIB_RefCount));
    ASMJIT_EMIT(a.inc(x86::dword_ptr(tempReg, indexReg, 2)));
    ASMJIT_EMIT(a.mov(tempReg, (uint64_t)&g_origAddResource));
    ASMJIT_EMIT(a.mov(tempReg, x86::ptr(tempReg)));
    ASMJIT_EMIT(a.add(tempReg, firstSize));
    // 顺序不可交换：必须先 pop 恢复 listReg，再 xchg 把跳回地址放到栈顶。
    ASMJIT_EMIT(a.pop(listReg));
    ASMJIT_EMIT(a.xchg(tempReg, x86::ptr(x86::rsp)));
    ASMJIT_EMIT(a.ret());

    return FinalizeAndInstall(hookAddr, &g_origAddResource, "AddResource", code, 2);
}

// IB/VB Release @ RVA 0x95664A（indexReg=r8, tempReg=r9；需要 firstSize）
static bool InstallHook_Release() {
    // 站点解析：RVA + AOB 兜底 + 字节校验（docs/PLUGIN_GUIDE.md §5.2）。
    // AOB 命中 add 指令起点，hook 点在其后 9 字节的 mov（见下方注释）。
    // RVA 必须指向【add 指令起点 0x956641】，与 kExpect 的字节对齐：
    //   0x956641  add dword [rbx+r8*4+0x80], -1   ← 42 83 84 83 80 00 00 00 FF
    //   0x95664A  mov esi, [rbx+r8*4+0x80]        ← 真正的 hook 点（+9）
    // 原先写 0x95664A，字节校验必然失败、每次都退化到 AOB 扫描。
    static constexpr HookInfo kSite = HookRva(
        0x956641, "42 83 84 83 80 00 00 00 FF 42 8B B4 83 80 00 00 00 75 5F", 1);
    static const uint8_t kExpect[] = { 0x42, 0x83, 0x84, 0x83, 0x80, 0x00, 0x00, 0x00, 0xFF };
    const uint64_t addAddr = ResolveSite(kSite, kExpect, sizeof(kExpect), "Release");
    const uint64_t hookAddr = addAddr ? addAddr + 9 : 0;
    if (!hookAddr) {
        return false;
    }
    // g_ibListVA = m_IBList（0x142B0A740）。命中则走 label2 用 g_pIB_RefCount，
    // 否则（rbx == m_VBList）用 g_pVB_RefCount。变量名原为 mVBList，属误称。
    const uint64_t mIBList = g_ibListVA;

    using namespace asmjit;
    CodeHolder code;
    code.init(rt.environment());
    x86::Assembler a(&code);
    x86::Gp indexReg = x86::r8;
    x86::Gp tempReg = x86::r9;
    x86::Gp listReg = x86::r10;   // 专用于 m_IBList 比较
    int firstSize = 1;
    {
        size_t len = 1;
        if (DecodeInsn(hookAddr, &len, nullptr)) firstSize = (int)len;
    }
    Label label2 = a.newLabel();

    ASMJIT_EMIT(a.push(tempReg));
    ASMJIT_EMIT(a.push(listReg));

    // hook 点 0x95664A 是 mov esi,[rbx+r8*4+0x80]（读）；真正的 -1 在它【之前】
    // 的 0x956641 已经执行过了（写【原始】数组；rbx 动态寻址无法静态重定向）。
    //
    // 【已移除 index 分流】所有句柄一律：先 +1 抵掉 0x956641 对原始数组的 -1，
    // 再把真正的 -1 落到扩展数组，esi 取扩展数组值。
    ASMJIT_EMIT(a.add(x86::dword_ptr(x86::rbx, indexReg, 2, 0x80), 1));

    // 【不要写 a.cmp(x86::rbx, (uint64_t)mIBList)】——asmjit 1.17 对 cmp r64,imm64
    // 的校验是 if (size == 8) { if (!is_int_n<32>(imm)) goto InvalidImmediate; }，
    // 而 InvalidImmediate 只写进 emitter 的局部 err，CodeHolder / JitRuntime::_add 都不检查：
    // add() 照样返回 kErrorOk，却把整条 cmp 静默丢掉，je 于是沿用前面指令的标志位，
    // IB 路径可能永不可达。必须拆成 movabs + cmp reg,reg（ASMJIT_EMIT 是第二道保险：
    // 将来若有人写回 imm64 形式，会在安装前报错而不是静默丢指令）。
    ASMJIT_EMIT(a.mov(listReg, (uint64_t)mIBList));
    ASMJIT_EMIT(a.cmp(x86::rbx, listReg));
    ASMJIT_EMIT(a.je(label2));
    ASMJIT_EMIT(a.mov(tempReg, (uint64_t)g_pVB_RefCount));
    ASMJIT_EMIT(a.add(x86::dword_ptr(tempReg, indexReg, 2), -1));
    ASMJIT_EMIT(a.mov(x86::esi, x86::ptr(tempReg, indexReg, 2)));
    ASMJIT_EMIT(a.mov(tempReg, (uint64_t)&g_origRelease));
    ASMJIT_EMIT(a.mov(tempReg, x86::ptr(tempReg)));
    // ADD 破坏 ZF：回到 0x956652 的 jnz 恒跳，不进 DeletionPipe（宁泄漏不误删）。
    ASMJIT_EMIT(a.add(tempReg, firstSize));
    // 顺序不可交换：必须先 pop 恢复 listReg，再 xchg 把跳回地址放到栈顶。
    ASMJIT_EMIT(a.pop(listReg));
    ASMJIT_EMIT(a.xchg(tempReg, x86::ptr(x86::rsp)));
    ASMJIT_EMIT(a.ret());

    a.bind(label2);
    ASMJIT_EMIT(a.mov(tempReg, (uint64_t)g_pIB_RefCount));
    ASMJIT_EMIT(a.add(x86::dword_ptr(tempReg, indexReg, 2), -1));
    ASMJIT_EMIT(a.mov(x86::esi, x86::ptr(tempReg, indexReg, 2)));
    ASMJIT_EMIT(a.mov(tempReg, (uint64_t)&g_origRelease));
    ASMJIT_EMIT(a.mov(tempReg, x86::ptr(tempReg)));
    ASMJIT_EMIT(a.add(tempReg, firstSize));
    // 顺序不可交换：必须先 pop 恢复 listReg，再 xchg 把跳回地址放到栈顶。
    ASMJIT_EMIT(a.pop(listReg));
    ASMJIT_EMIT(a.xchg(tempReg, x86::ptr(x86::rsp)));
    ASMJIT_EMIT(a.ret());

    return FinalizeAndInstall(hookAddr, &g_origRelease, "Release", code, 3);
}

// ---------------------------------------------------------------------------
// 固定字节补丁（照抄 DllProxy 行 1178-1193，顺序与内容不变）
// 地址经 ResolveExe 解析；失败只 Log、继续（不中断）。SPIMode 开关不移植
// （固定全开）。
// ---------------------------------------------------------------------------
static void PatchLimitByte(uintptr_t rva, uint8_t byte) {
    uint64_t addr = ResolveExe(rva, 4);
    if (!addr) addr = SiteFallback(rva);
    if (!addr) {
        g_api->Log("UnlockModelLimit: RVA 0x%X unavailable", (unsigned int)rva);
        return;
    }
    const std::vector<uint8_t> bytes = { byte };
    PatchBytes(addr, bytes);
}

static void ApplyLimitBytePatches() {
    // IB/VB AddResource 硬上限：cmp eax, 2000h (8192) -> 8000h (32768)
    PatchLimitByte(0x9407CF, 0x40);   // cmp eax, 2000h @ 9407CD (imm byte2)
    PatchLimitByte(0x9407FE, 0x40);   // mov edx, 2000h @ 9407FC (imm byte2)
    PatchLimitByte(0x940864, 0x40);   // mov r8d, 2000h @ 940861（41 B8 00 20 00 00：带 REX.B 前缀，
                                      //   立即数第 2 字节在 0x940864；原 0x940863 会写成 imm=0x2040）
    PatchLimitByte(0x9417EB, 0x04);   // AllocateStreamInstanceVB 的 D3D11_BUFFER_DESC.ByteWidth = 0x20000
                                      //   （8192 实例 x 16 字节）。模型上限翻倍（8192->16384）后该缓冲
                                      //   必须翻倍：0x20000 -> 0x40000（立即数第 3 字节 02 -> 04）。

    // SPI AddResource 硬上限：cmp eax, 4000h (16384) -> 8000h (32768)
    PatchLimitByte(0x940E5F, 0x80);   // cmp eax, 4000h @ 940E5D (imm byte2)
    PatchLimitByte(0x940E8E, 0x80);   // mov edx, 4000h @ 940E8C (imm byte2)
    PatchLimitByte(0x940EF4, 0x80);   // mov r8d, 4000h @ 940EF1（41 B8 00 40 00 00：带 REX.B 前缀，
                                      //   立即数第 2 字节在 0x940EF4；原 0x940EF3 会写成 imm=0x4080）
}

// DX12 分支的入口（实现在 UnlockModelLimit_Dx12.cpp）
void UnlockModelLimit_Dx12_Init();


// ---------------------------------------------------------------------------
// 诊断 + 迁移（loader/exe 异步竞态取证）
// Civ6DllLoader 与 exe 并发运行：exe 主流程在插件打补丁之前就已启动
// （实测 Startup.log 的 InitialInit 早于插件 install 约 1-2 秒）。若 exe 在
// 补丁挂上之前创建了 IB/VB/SPI 资源，它们会写进【原始】数组；补丁后所有
// 读取都改走【扩展】数组 -> 早期资源"消失"（读到全零）-> 空描述符 ->
// Layout_UploadBuffers 0x951120 解引用 NULL 崩溃。
// 此处在打任何补丁【之前】执行：
//   1) 取证：读各列表 m_cItems + 扫描 8 个原始数组的非零计数（写入日志）
//   2) 修复：若检测到数据，把原始数组内容 memcpy 进扩展数组前段（迁移），
//      使 pre-patch 资源对重定向后的读路径可见。全零时迁移是无操作。
// ---------------------------------------------------------------------------
static void DiagnoseAndMigrateOriginalArrays() {
    // ---- 1) 取证：列表 m_cItems（u16 @ 列表对象 +0）----
    struct ListDef { uintptr_t rva; const char* name; };
    const ListDef lists[] = {
        {g_ibListVA, "m_IBList"},
        {g_vbListVA, "m_VBList"},
        {g_spiListVA, "m_SPIList"},
        {g_computeSpiListVA, "m_ComputeSPIList"},
    };
    for (const ListDef& d : lists) {
        const uint64_t a = ResolveExe(d.rva, 8);
        unsigned cItems = 0;
        if (a) cItems = *(const unsigned short*)a;
        g_api->Log("[MIGRATE] %s.m_cItems = %u (pre-patch)", d.name, cItems);
    }

    // ---- 2) 取证：原始数组非零 qword 计数 ----
    struct ArrDef { uintptr_t rva; size_t bytes; const char* name; };
    const ArrDef defs[] = {
        {(g_ibListVA + 0x80), 8192 * 4,    "IB.refcount"},
        {(g_ibListVA + 0x8540), 8192 * 8,    "IB.resources"},
        {(g_ibListVA + 0x18680), 8192 * 0x30, "IB.extra"},
        {(g_vbListVA + 0x80), 8192 * 4,    "VB.refcount"},
        {(g_vbListVA + 0x8540), 8192 * 8,    "VB.resources"},
        {(g_vbListVA + 0x18680), 8192 * 0x30, "VB.extra"},
        {(g_spiListVA + 0x80), 16384 * 4,   "SPI.refcount"},
        {(g_spiListVA + 0x104C0), 16384 * 16,  "SPI.resources"},
    };
    bool anyNonZero = false;
    for (const ArrDef& d : defs) {
        const uint64_t a = ResolveExe(d.rva, 8);
        if (!a) continue;
        const uint64_t* p = (const uint64_t*)a;
        const size_t n = d.bytes / 8;
        size_t nz = 0;
        size_t last = 0;
        for (size_t i = 0; i < n; ++i) {
            if (p[i]) { ++nz; last = i; }
        }
        if (nz) anyNonZero = true;
        g_api->Log("[MIGRATE] %s: %zu/%zu nonzero qwords%s",
            d.name, nz, n, nz ? " (races confirmed on this array)" : "");
        if (nz) {
            g_api->Log("[MIGRATE]   last nonzero @ +%zX", last * 8);
        }
    }

    if (!anyNonZero) {
        g_api->Log("[MIGRATE] pre-patch scan: all original arrays zero - exe did NOT race ahead");
        return;
    }

    // ---- 3) 迁移：原始数组 -> 扩展数组（仅已有数据，全零槽位拷贝无害）----
    g_api->Log("[MIGRATE] pre-patch data detected - migrating original -> extended arrays");
    struct CopyDef { void* dst; uintptr_t srcRva; size_t bytes; const char* name; };
    const CopyDef copies[] = {
        {g_pIB_RefCount,        (g_ibListVA + 0x80), 8192 * 4,    "IB.refcount"},
        {g_pIB_Resources,       (g_ibListVA + 0x8540), 8192 * 8,    "IB.resources"},
        {g_pIB_ExtraData,       (g_ibListVA + 0x18680), 8192 * 0x30, "IB.extra"},
        {g_pVB_RefCount,        (g_vbListVA + 0x80), 8192 * 4,    "VB.refcount"},
        {g_pVB_Resources,       (g_vbListVA + 0x8540), 8192 * 8,    "VB.resources"},
        {g_pVB_ExtraData,       (g_vbListVA + 0x18680), 8192 * 0x30, "VB.extra"},
        {g_pSPIList_RefCount,   (g_spiListVA + 0x80), 16384 * 4,   "SPI.refcount"},
        {g_pSPIList_Resources,  (g_spiListVA + 0x104C0), 16384 * 16,  "SPI.resources"},
    };
    for (const CopyDef& c : copies) {
        const uint64_t src = ResolveExe(c.srcRva, 8);
        if (!c.dst || !src) continue;
        memcpy(c.dst, (const void*)src, c.bytes);
        g_api->Log("[MIGRATE] copied %zu bytes %s -> %p", c.bytes, c.name, c.dst);
    }
    g_api->Log("[MIGRATE] migration complete");
}


// ---------------------------------------------------------------------------
// 入口 / 出口
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// 检测第三方 "Assets Limit Fix"（DatabaseDLL 代理形态）。
//
// 为什么必须检测：那个 mod 用假的 DatabaseDLL_FinalRelease.dll 顶替原版，加载时机
// 远早于本插件（DatabaseDLL 在 exe 静态导入表里，version.dll 在它之后）。它把同一批
// 站点（0x946A37 / 0x957990 / 0x956B15 / 0x95D775 / 0x94D013 / 0x95CEB5 / 0x94ABAC /
// 0x9504FB / 0x9506F9 / 0x950757 / 0x940542）改成指向它自己的 65535 容量数组。
// 我们再改这些点只会互相覆盖：它的数组仍然存在但没人指向，而它的 4 张校验表
// 是在我们之前跑的、不会重跑 —— 结果既不报错也不生效，比单开任何一个都糟。
//
// 判定方式：扫描该模块映像里它独有的签名字符串。原版 DatabaseDLL 由 Firaxis 编译，
// 不可能包含 "CIV6-ASSETS-LIMIT-FIX-PROXY"；实测游戏目录原版 DLL 里该串不存在。
// 不依赖导出（它为了冒充原版，导出表必须与原版逐项一致，不能加自己的符号）。
// ---------------------------------------------------------------------------
static bool DetectAssetsLimitFix() {
    HMODULE h = GetModuleHandleW(L"DatabaseDLL_FinalRelease.dll");
    if (!h) {
        return false;
    }
    // 在模块的只读数据范围内找签名。用 PE 节表定位比扫全映像快且不会误触不可读页。
    const uint8_t* base = reinterpret_cast<const uint8_t*>(h);
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    static const char kSig[] = "CIV6-ASSETS-LIMIT-FIX-PROXY";
    const size_t sigLen = sizeof(kSig) - 1;   // 不含结尾 NUL

    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        // 只扫已初始化数据节：签名在 .rdata
        if (!(sec->Characteristics & IMAGE_SCN_CNT_INITIALIZED_DATA)) continue;
        const uint8_t* p = base + sec->VirtualAddress;
        const size_t n = sec->Misc.VirtualSize;
        if (n < sigLen) continue;
        for (size_t k = 0; k + sigLen <= n; ++k) {
            if (p[k] != (uint8_t)kSig[0]) continue;
            if (memcmp(p + k, kSig, sigLen) == 0) {
                return true;
            }
        }
    }
    return false;
}

void UnlockModelLimit_Init(HookAPI* api) {
    // api 是调用方 HookAPI_Resolve 出来的本模块私有表，非空即已解析。
    // 兼容性与依赖项检查由 OnLoad 的 HookContext_IsCompatible + HookAPI_RequireAll 负责。
    if (!api) {
        return;
    }
    g_api = api;

    // 前置判断：本插件同时支持 DX11 与 DX12 两份 exe —— 它们是完全不同的二进制
    // （RVA、ResourceList 布局、站点集合都不同），必须先判定当前进程加载的是哪一个，
    // 后续所有地址都经 ResolveExe 走这张映像。
    g_exeImage = HookCurrentExeImage(api);
    if (g_exeImage == kHookImageInvalid) {
        api->Log("UnlockModelLimit: no supported exe image loaded (DX11/DX12 both absent)");
        return;
    }
    api->Log("UnlockModelLimit: exe image = %s",
        g_exeImage == kHookImageExeDx12 ? "DX12" : "DX11");

    // 【互斥检查】第三方 "Assets Limit Fix"（假 DatabaseDLL）占了同一批站点，
    // 且它的加载时机早于本插件。两者同时生效只会互相覆盖 —— 它的 65535 容量数组
    // 被我们的 32768 补丁顶掉指针，而它的校验表早已跑完不会重报错。
    // 检测到就整体让位：不分配、不打补丁、不装 hook，保持它那份完整生效。
    if (DetectAssetsLimitFix()) {
        api->Log("UnlockModelLimit: 检测到 Assets Limit Fix (DatabaseDLL 已被替换) —— "
                 "两者改同一批站点，本插件整体让位，不做任何修改。"
                 "如需本插件生效，请删除游戏目录的 DatabaseDLL_FinalRelease.dll 并恢复原版。");
        return;
    }

    if (g_exeImage == kHookImageExeDx12) {
        // DX12 分支全部实现在 UnlockModelLimit_Dx12.cpp
        UnlockModelLimit_Dx12_Init();
        return;
    }

    if (!ResolveListBases()) {
        return;
    }
    if (!InitSPIExtendedArrays()) {
        return;
    }
    if (!InitIBVBExtendedArrays()) {
        return;
    }

    // 必须在【任何】补丁/hook 挂上之前执行：取证 + 迁移 pre-patch 数据
    DiagnoseAndMigrateOriginalArrays();

    ApplySPIRedirectPatches();
    ApplyIBVBBasePatches();
    ApplyIBVBIndexedPatches();

    const bool ok1 = InstallHook_AddResourceSPI();
    const bool ok2 = InstallHook_ReleaseSPI();
    const bool ok3 = InstallHook_AddResource();
    const bool ok4 = InstallHook_Release();
    const int hooksOk = (ok1 ? 1 : 0) + (ok2 ? 1 : 0) + (ok3 ? 1 : 0) + (ok4 ? 1 : 0);

    ApplyLimitBytePatches();

    g_api->Log("UnlockModelLimit: installed (%zu patches, %d/4 hooks)",
        g_patchedAddrs.size(), hooksOk);

}

