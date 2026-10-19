// UnlockModelLimit_Dx12.cpp
// DX12 分支实现（从 UnlockModelLimit.cpp 分离）。
//
// 为什么独立成文件：DX11 与 DX12 是两份完全不同的二进制 ——
//   - RVA 完全不同
//   - ResourceList 布局完全不同（见 docs/DX12_RESOURCELIST_LAYOUT.md）
//   - 站点集合不同（DX11 的 Unlock/LockDynamic 一族在 DX12 根本不存在）
// 混在一个文件里改一边会很容易误伤另一边。
//
// 共享的基础设施（PatchBytes / DecodeInsn / ResolveExe / FinalizeAndInstall /
// g_api / rt / g_hookSlots / g_exeImage / g_patchedAddrs）在 UnlockModelLimit_Common.h。
//
// 与 DX11 的关键差异：
//   1. 【整条指令替换】不再手算 disp32 增量 —— 见 dx12_ReplaceInsn / dx12_AsmMemInsn。
//      增量法必须记得减掉指令里固有的偏移（如 0x80），漏减会让落点整体偏移，
//      这是个已经踩过的坑。整条替换时 disp32 是"目标地址 − 基址"直接算出的，
//      不存在中间量可以搞错。
//   2. DX12 的 m_Resources 是 40 字节步长的"资源记录"数组（+0x8400），
//      不是 DX11 的 8 字节指针数组（+0x8540）。
//   3. ExtraData 是列表内数组（+0x58540，步长 0x70），不是独立对象。
//   4. m_DeletionPipe 是 3 条（DX11 是 4 条）。

#include "pch.h"
#include "HookAPI.h"
#include "UnlockModelLimit.h"
#include "UnlockModelLimit_Common.h"
#include "asmjit/asmjit.h"
#include "Zydis/Zydis.h"
#include <vector>
#include <cstring>
#include <malloc.h>   // _aligned_malloc / _aligned_free（与引擎 aligned_malloc/free 同源）
#include <cstdint>
#include <algorithm>

// 站点 AOB 表（可选生成物）：存在时给所有 patch 站点启用 AOB 兜底。
#if __has_include("UnlockModelLimit_Dx12_Sites.h")
#include "UnlockModelLimit_Dx12_Sites.h"
#define DX12_HAS_SITE_AOB 1
#else
#define DX12_HAS_SITE_AOB 0
#endif

namespace {
// DX12 扩展数组（<2GB，见 dx12_InitExtendedArrays）
}  // namespace

// ===========================================================================
// DX12 分支
// ===========================================================================
// 依据：docs/DX12_UNLOCKMODELLIMIT_ANALYSIS.md（IDA 实机取证）。
//
// DX12 与 DX11 是两份不同的二进制，ResourceList 布局【完全不同】：
//
//   DX11 ResourceList<ID3D11Buffer*,8192,128,4,ushort>  sizeof 0x18680
//     +0x0000 m_cItems        +0x8080 m_uCurPipe   +0x8540 m_Resources[8192] (8B/项)
//     +0x0040 m_lock          +0x8088 m_DeletionPipe[4] (0x128/项)
//     +0x0080 m_RefCount[8192] (4B/项)  +0x18540 m_DeleteList  +0x18668 m_RecycleList
//     ExtraData[8192] 在列表对象【之外】(ResourceListExtraData，+0x18680, 0x30/项)
//
//   DX12 ResourceList<ID3D11Buffer*,8192,128,4,ushort>  sizeof 0x58540
//     +0x0000 m_cItems        +0x8080 m_uCurPipe   +0x8400 资源记录[8192] (0x28/项)
//     +0x0040 m_lock          +0x8088 m_DeletionPipe[3] (0x128/项)
//     +0x0080 m_RefCount[8192] (4B/项)  +0x58400 m_DeleteList  +0x58528 m_RecycleList
//     ExtraData[8192] 紧跟列表对象之后 (+0x58540, 0x70/项)
//
// 三个关键差异：
//   1. DX12 没有 m_Resources 指针数组；取而代之的是 +0x8400 起、步长 0x28 的"资源记录"数组。
//      所以 DX11 那些 [reg+idx*8+0x8540] 形态的站点在 DX12 里【不存在】。
//   2. DX12 的 ExtraData 步长是 0x70（不是 DX11 的 0x30）。
//   3. DX12 的 SPI 一族布局与 DX11【一致】（0x50900），只有基址不同。
//
// 因此 DX12 需要重定向的是「资源记录数组 / ExtraData 数组 / SPI 的 Resources+RefCount」三类，
// 而不是 DX11 的「Resources 指针数组 / ExtraData 数组」两类。
//
// 已确认在 DX12 【不存在】的 DX11 站点（不要移植）：
//   UnlockIB / UnlockVB / LockDynamicIB / LockDynamicVB / LockAndFillStaticIB / LockAndFillStaticVB /
//   RenderSurface 的 ExtraData.iInputFormat —— D3D11 的 Map/Unmap/UpdateSubresource 动态缓冲
//   路径在 DX12 里整体消失（DX12 二进制中连 "d3d11" 字样都没有，只有 d3d12.dll）。
// ===========================================================================

// DX12 的 IB/VB 原始上限（AddResource/Release hook 的 cmp 阈值）
static const int ORIG_LIMIT = 8192;

namespace dx12 {

// ---- 列表基址：运行期解析（见 dx12_ResolveListBases）----
// .data 段的绝对地址 PdataMatch 校正不了（.pdata 只覆盖函数），所以不能写死 RVA。
// 改成从引用列表的 lea 指令里解 disp32：位置随版本变，指令序列不变。
inline uint64_t g_ibListVA = 0;
inline uint64_t g_vbListVA = 0;
inline uint64_t g_spiListVA = 0;
inline uint64_t g_computeSpiListVA = 0;

// 三处 AddRef 实例的指令序列逐字节相同（只有 disp32 不同），
// 按地址升序依次是 IB / SPI / VB —— 用 index 区分。
// 目标段必须单独成段（xx 紧贴段末），否则 ResolveDataByAob 的 E 会算到后面的指令上。
constexpr char kAob_AddRefList[] =
    "48 8D 0D xx xx xx xx | 48 8D 0C 81 8B 81 80 00 00 00 FF C0 89 81 80 00 00";
// CreateComputePackageInstance 的 lea 引用 m_ComputeSPIList。
constexpr char kAob_ComputeSPIList[] =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B D9 8B F2 | 48 8D 2D xx xx xx xx";

// ---- 列表内字段偏移 ----
constexpr size_t   kRecordStride     = 0x28;
constexpr size_t   kExtraStride      = 0x70;

// SPI 一族（布局与 DX11 相同）
constexpr uint32_t kSpiOffRefCount   = 0x0080;

// ---- 上限字节补丁（全部写"立即数第 2 字节"）----
// IB/VB: cmp eax,2000h / mov edx,2000h / mov r8d,2000h  -> 4000h
// SPI  : cmp eax,4000h / mov edx,4000h / mov r8d,4000h  -> 8000h
constexpr uintptr_t kPatchIBCmp      = 0x940C3F;   // 原 0x20 -> 0x40
constexpr uintptr_t kPatchIBMovEdx   = 0x940C6E;   // 原 0x20 -> 0x40
constexpr uintptr_t kPatchIBMovR8d   = 0x940CD4;   // 原 0x20 -> 0x40（41 B8 前缀，故 +3）
constexpr uintptr_t kPatchSPICmp     = 0x9412CF;   // 原 0x40 -> 0x80
constexpr uintptr_t kPatchSPIMovEdx  = 0x9412FE;   // 原 0x40 -> 0x80
constexpr uintptr_t kPatchSPIMovR8d  = 0x941364;   // 原 0x40 -> 0x80（41 B8 前缀，故 +3）

// ---- 4 个 hook 站点（与 DX11 对应站点逐字节同码）----
// IB/VB 两处 detour 的站点、AOB 与跳回点见 dx12_InstallHooks()。
// SPI refcount 三条指令（原地 disp32 改写用；见 dx12_ApplyRedirectPatches 说明）
// ---- SPI 真身站点（用回收链表偏移确认归属）----
// m_SPIList (ResourceList<FGXShaderPackageInstance,16384,512,1,ushort>) 布局：
//   +0x0      m_cItems
//   +0x40     m_lock
//   +0x80     m_RefCount[16384]
//   +0xA1128  m_DeleteList   (+0x410 ptr / +0x418 cap / +0x420 count)
//   +0xA1140  m_RecycleList
// 判据：m_SPIList 的 Release 用 [rbx+0xA1128h]，而 sub_14094D9B0 用 [rbx+0x8080h]
//       —— 后者是另一个列表（m_ComputeSPIList 一侧），不是 SPI。
//
// ① AddRefShaderPackageInstance (sub_1409407E0)：伪基址改写，已有 kSpiSiteAddRef=0x940802 ✓
//    它只服务 m_SPIList（锁是绝对地址 g_SPIList.m_lock），无需分流。
// ② sub_14094DC20 是 ReleaseShaderPackageInstance 的另一份同名实现，没有任何代码调用者
//    （xref 只有 data 段），其 refcount 指令（0x94DC61 / 0x94DC6A）不需要 patch。
// ③ 真正遗漏的：Renderer::Shutdown 的全表清理 sub_1409454D0
//    它扫 m_cItems 并读/清 m_RefCount[idx]，而 m_RefCount 已被我们重定向到新数组。
//    不 patch 的后果：它扫【旧】数组（永远全 0）→ 回收逻辑一次不执行 →
//    m_DeleteList 永不填充 → 句柄泄漏，最终数组耗尽。
//    字节：39 ac 86 80 00 00 00   cmp dword [rsi+rax*4+80h], ebp   (disp32 @ +3)
//          89 ac 86 80 00 00 00   mov dword [rsi+rax*4+80h], ebp   (disp32 @ +3)
constexpr uintptr_t kSpiShutCmp = 0x945503;
constexpr uintptr_t kSpiShutMov = 0x945513;

// ④ DX12 退出必崩的根因：IB/VB 版的全表清理 sub_140945160
//    字节与 SPI 版【完全同构】（连寄存器都一样，纯属巧合）：
//      0x945193  39 ac 86 80 00 00 00   cmp dword [rsi+rax*4+80h], ebp
//      0x9451A3  89 ac 86 80 00 00 00   mov dword [rsi+rax*4+80h], ebp
//    形态是 ModRM+SIB 的【硬编码 disp32】，不是 lea 的 RIP 相对，
//    所以 dx12_PatchRvaDisp 一个都覆盖不到 —— 之前整整漏了这一对。
//
//    崩溃机理（已逐条验证）：
//      1. AddResource hook 把 refcount 写进【新数组】g_p*RefCount；
//      2. 本函数读【旧数组】m_List+0x80+idx*4；
//      3. Reset(0x971490) 的 memset 长度仍是旧容量 0x8000（=8192*4），
//         8192..m_cItems-1 这一段【从未被写过】，是未初始化的堆垃圾；
//      4. 垃圾非零 -> 垃圾句柄被推进 m_DeleteList；
//      5. Reap(0x96E100) 拿垃圾句柄索引 ExtraData -> f50==0
//         -> sub_140970250 解引用 [0+0x40] -> 0xC0000005。
//    这也解释了诊断里 ExtraData[8192] 的 slotNonZeroDwords=0/28：
//    该槽位根本没被 CreateVBAtHandle 碰过，句柄是凭空冒出来的。
// 这两个 RVA 只用于文档/核对，【不打字节补丁】—— 原因见
//   dx12_InstallShutdownCleanupHook()：两个列表的 disp 不同，一条指令表达不了，
//   必须在函数入口按 rsi 分流。保留常量是为了让后来人能一眼定位到这两条指令。
[[maybe_unused]] constexpr uintptr_t kIBVBShutCmp = 0x945193;
[[maybe_unused]] constexpr uintptr_t kIBVBShutMov = 0x9451A3;

constexpr uintptr_t kSpiHookAdd       = 0x9413A1;   // inc dword [rbx+rcx*4+80h]
constexpr uintptr_t kSpiHookRelAdd    = 0x94DAC1;   // add dword [rbx+r8*4+80h],-1
constexpr uintptr_t kSpiHookRelMov    = 0x94DACA;   // mov esi,[rbx+r8*4+80h]

// ---- SPI 重定向站点 ----
// kind0 = lea r64,[rip+disp32] -> mov r64, imm32（7 字节）
// kind1 = mov r32,[idx*8+disp32]（8 字节，需把 SIB 的 base 去掉变绝对寻址）
constexpr uintptr_t kSpiSiteCreateSPI    = 0x944A77;   // lea rax, m_SPIList.m_Resources
constexpr uintptr_t kSpiSiteRenderSurface= 0x95E20A;   // lea rax, m_SPIList.m_Resources
constexpr uintptr_t kSpiSiteReleaseSPI   = 0x94DD35;   // lea rax, m_SPIList.m_Resources
constexpr uintptr_t kSpiSiteShutdownRes  = 0x97175B;   // lea rcx, m_SPIList.m_Resources
constexpr uintptr_t kSpiSiteShutdownRef  = 0x97177F;   // sub_140971490(Reset): lea rcx, m_SPIList.m_RefCount
// 注意：这里必须是【lea 指令自身】的地址，不是函数入口。
//   AddRef（sub_1409407E0）的 lea 在 +0x22 = 0x940802
//   GetEnum（sub_1409479E0）的 lea 在 +0x3  = 0x9479E3
constexpr uintptr_t kSpiSiteAddRef       = 0x940802;   // lea rcx, m_SPIList（伪基址 = RefCount-0x80）
constexpr uintptr_t kSpiSiteGetEnum      = 0x9479E3;   // lea rcx, m_SPIList.m_Resources.m_iPackageID
// DX11 的 4 个 disp32 站点（0x94ABAC/0x9504FB/0x9506F9/0x950757）在 DX12 收敛为这 1 处：
constexpr uintptr_t kSpiSitePkgIdDisp    = 0x95C2E3;   // mov eax,[r13+rax*8+0x6C2E904]

// ---- IB/VB 重定向站点（lea 形态）----
constexpr uintptr_t kIBRecCreate   = 0x96A4FA;   // CreateIBAtHandle: lea rcx, 记录数组
constexpr uintptr_t kIBExtCreate   = 0x96A51E;   // CreateIBAtHandle: lea rax, ExtraData
constexpr uintptr_t kVBRecCreate   = 0x96CD79;   // CreateVBAtHandle: lea rax, 记录数组
constexpr uintptr_t kVBExtCreate   = 0x96CD88;   // CreateVBAtHandle: lea rax, ExtraData
constexpr uintptr_t kIBRecRelease  = 0x94A565;   // sub_14094A550: lea rcx, 记录数组
constexpr uintptr_t kIBExtRelease  = 0x94A553;   // sub_14094A550: lea rcx, ExtraData
constexpr uintptr_t kVBRecRelease  = 0x94A6C5;   // sub_14094A6B0: lea rcx, 记录数组
constexpr uintptr_t kVBExtRelease  = 0x94A6B3;   // sub_14094A6B0: lea rcx, ExtraData
// Reap 一族是 SIB+无base 形态（49 8D 9C 24 <disp32> / 49 8D BC 24 <disp32>），
// disp32 本身就是绝对地址，patch 只需改 disp32（4 字节），指令长度不变。
constexpr uintptr_t kIBRecReap     = 0x96E1ED;   // sub_14096E100: lea rbx, [r12+记录数组]
constexpr uintptr_t kIBExtReap     = 0x96E1C7;   // sub_14096E100: lea rdi, [r12+ExtraData]
constexpr uintptr_t kVBRecReap     = 0x96E16D;   // sub_14096E100: lea rbx, [r12+记录数组]
constexpr uintptr_t kVBExtReap     = 0x96E147;   // sub_14096E100: lea rdi, [r12+ExtraData]
constexpr uintptr_t kIBRecReset    = 0x971658;   // sub_140971490: lea rcx, 记录数组
constexpr uintptr_t kVBRecReset    = 0x9715F2;   // sub_140971490: lea rcx, 记录数组
constexpr uintptr_t kIBRefReset    = 0x971666;   // sub_140971490: lea rcx, m_RefCount
constexpr uintptr_t kVBRefReset    = 0x971600;   // sub_140971490: lea rcx, m_RefCount
// Reset 里紧跟 lea 的 "mov r8d, <memset 长度>"：长度只覆盖【旧】容量。
// 重定向后插件数组是 65536 项，不清满会让高索引的引用计数跨局残留。
constexpr uintptr_t kIBRefResetLen  = 0x97166D;   // mov r8d, 8000h  (= 8192 项)
constexpr uintptr_t kVBRefResetLen  = 0x971607;   // mov r8d, 8000h  (= 8192 项)
constexpr uintptr_t kSPIRefResetLen = 0x971786;   // mov r8d, 10000h (= 16384 项)
// sub_14096C250 的两处：记录数组是 SIB+无base（4D 8D A0 <disp32>），ExtraData 是 RIP 相对（48 8D 05）。
constexpr uintptr_t kVBRecRelease2 = 0x96C297;   // lea r12, [r8+记录数组]
constexpr uintptr_t kVBExtRelease2 = 0x96C2AC;   // lea rax, [rip+ExtraData]

// ---- IB/VB："列表基址 + 偏移" 形态（不是绝对地址，xrefs_to 找不到！）----
// sub_14094A470 (ReleaseIB) / sub_14094A4D0 (ReleaseVB) 里：
//     lea rcx, [rcx+rax*8]     ; rcx = m_IBList, rax = 5*idx
//     add rcx, 8400h           ; 48 81 C1 00 84 00 00
// 这里 rcx 最终 = &m_IBList[5*idx + 0x2100]，即【原始】记录数组里的元素。
// 必须把 imm32 从 0x8400 改成 (新记录数组 - 列表基址)，否则它读原始数组、
// 而写入方（CreateIBAtHandle）写的是新数组 —— 读到垃圾指针后 memcpy 必崩。
// ---- AddRefIB/AddRefVB：整个 RefCount 数组必须重定向 ----
// DX12 的 AddRef（sub_140940790 / sub_140940880）：
//     lea rcx, m_IBList           <- 必须换成 RefCount 数组基址
//     lea rcx, [rcx+rax*4]
//     mov eax, [rcx+80h] / inc / mov [rcx+80h], eax
// 若不改，refcount 自增落在【原始】数组上，而 AddResource/Release 的 hook 用的是
// 新数组 —— 两个数组计数不一致，资源被提前释放或永不释放。
// （DX11 对应站点 0x9404F2 / 0x9405E2，改成 ref-0x80 的伪基址。）
// ⚠ 这两处是基址+偏移形式，xrefs_to 找不到，之前的普查也漏了。
constexpr uintptr_t kIBAddRefBase = 0x9407B2;   // lea rcx, m_IBList
constexpr uintptr_t kVBAddRefBase = 0x9408A2;   // lea rcx, m_VBList

// ---- ReleaseIB/ReleaseVB 的 ExtraData 访问（同样漏了）----
//     lea rdx, [rcx+58540h]       <- ExtraData 数组基址
//     mov edi, [rax+rcx+585A8h]   <- ExtraData[idx].field@0x68
constexpr uintptr_t kIBRelExtBase = 0x94A48C;   // lea rdx, [rcx+58540h]
constexpr uintptr_t kIBRelExt68   = 0x94A496;   // mov edi, [rax+rcx+585A8h]
constexpr uintptr_t kVBRelExtBase = 0x94A4EC;   // lea rdx, [rcx+58540h]
constexpr uintptr_t kVBRelExt68   = 0x94A4F6;   // mov edi, [rax+rcx+585A8h]

// ---- SetVbIb（sub_14095F5E0）里的【记录字段】读取 ----
// 形态：`mov eax, [reg+rsi+disp32]`，rsi = imagebase，disp32 = 记录数组内字段的绝对地址。
// 这些 disp32 指向【原始】记录数组的字段，而 CreateIBAtHandle 写的是【新】数组 —— 必须一起改。
// ⚠ 这 5 处同样是"xrefs_to 找不到"的形态：它们引用的是记录数组【内部字段】的地址，
//    而不是数组基址，所以按基址查 xref 会全部漏掉。
constexpr uintptr_t kIBRecF1C = 0x95F643;   // mov eax,[rcx+rsi+6C7A45Ch]
constexpr uintptr_t kIBRecF18 = 0x95F653;   // mov eax,[rcx+rsi+6C7A458h]
constexpr uintptr_t kVBRecF1C = 0x95F6F2;   // mov eax,[rbx+rsi+0F2BD4DCh]
constexpr uintptr_t kVBRecF18 = 0x95F6FD;   // mov eax,[rbx+rsi+0F2BD4D8h]
constexpr uintptr_t kVBRecF20 = 0x95F71F;   // mov eax,[rbx+rsi+0F2BD4E0h]

// ---- records 数组的【其余字段】读取（f10/f28/f40/f44/f110）----
// 这些字段的【写入方】走 CreateIBAtHandle 派生出的指针（v5+16 等），
// 所以只要 0x96A4FA 那处补丁生效，新数组里这些字段就会被正确写入。
// 但【读取方】用的是硬编码绝对地址（如 qword_146C7A450），必须一起改，
// 否则读旧数组 -> 拿到 0 -> 崩（0x14095FF14 就是实际崩溃点）。
//
// 字节形态（三种，均可由 dx12_PatchRvaDisp 的增量法处理）：
//   mov r64, [rbx+idx*8+disp32]   48 8B 94 D3 <disp32>
//   mov r32, [rbx+idx*8+disp32]   8B 84 C3 <disp32>
//   mov r32, cs:disp32            8B 05 <disp32>      （无索引，直接绝对）
//   lea r64, [rip+disp32]         48 8D 1D <disp32>
constexpr uintptr_t kIBRecF10  = 0x95FF14;   // mov rdx, qword_146C7A450[rbx+rdx*8]  【崩溃点】
constexpr uintptr_t kVBRecF10  = 0x95FEF7;   // mov rdx, qword_14F2BD4D0[rbx+rdx*8]
constexpr uintptr_t kIBRecF28  = 0x95D51B;   // mov rax, qword_146C7A468[rbx+rax*8]
constexpr uintptr_t kVBRecF28  = 0x95D4E1;   // mov eax, qword_14F2BD4E8[rbx+rax*8]
constexpr uintptr_t kIBRecF40  = 0x95D530;   // mov eax, cs:dword_146C7A480
constexpr uintptr_t kIBRecF44  = 0x95D527;   // mov eax, cs:dword_146C7A484
constexpr uintptr_t kVBRecF40  = 0x95D4F5;   // mov eax, cs:dword_14F2BD500
constexpr uintptr_t kVBRecF44  = 0x95D4EC;   // mov eax, cs:dword_14F2BD504
// 【不要 patch】kIBRecF110 = 0xF99BD3：这是析构函数的哨兵起跑线（m_DeletionPipe[3].pBlock），
//   不是记录字段。见 dx12_ApplyRedirectPatches 里的说明。
// 【不要 patch】kVBRecF110 = 0xF99EE3：同上（VB 版析构）。

constexpr uintptr_t kIBRecBaseAdd = 0x94A4A5;   // add rcx, 8400h  (ReleaseIB)
constexpr uintptr_t kVBRecBaseAdd = 0x94A505;   // add rcx, 8400h  (ReleaseVB)

// ---- IB/VB 重定向站点（disp32 形态，base 必须去掉）----
constexpr uintptr_t kIBRecIndex    = 0x95F62A;   // mov rax, rva 记录数组[rsi+rcx*8]
constexpr uintptr_t kVBIxIndex     = 0x95F6D7;   // mov rax, rva 记录数组[rsi+rcx*8]

// ---- 数组容量配置 ----
// ⚠ 实测：DX12 的 SPI 索引曾达到 40960，超出原先照搬 DX11 的 32768 项。
// m_cItems 是 u16（最大 65535），所以数组最多 65536 项。
// 先按 65536 分配（u16 句柄可寻址全部 0..65535），上限补丁也提到 0xFFFE。
constexpr size_t kArrayItems = 65536;   // 原 32768

// ---- 扩展数组（插件侧，全部要求 < 2GB）----
static uint32_t* g_pIBRefCount = nullptr;   // u32[32768]
static uint8_t*  g_pIBRecords  = nullptr;   // 记录[32768]，步长 0x28
static uint8_t*  g_pIBExtra    = nullptr;   // ExtraData[32768]，步长 0x70
static uint32_t* g_pVBRefCount = nullptr;
static uint8_t*  g_pVBRecords  = nullptr;
static uint8_t*  g_pVBExtra    = nullptr;
static uint32_t* g_pSPIRefCount = nullptr;  // u32[32768]
static uint8_t*  g_pSPIRes     = nullptr;   // FGXShaderPackageInstance[32768]，步长 16


}  // namespace dx12

// ===========================================================================
// DX12 分支实现
// ===========================================================================

// DX12 扩展数组分配。布局（每列表）：
//   RefCount[32768]*4   + 记录[32768]*0x28 + ExtraData[32768]*0x70
// 两个列表（IB+VB）各一份，外加 SPI 的 Resources[32768]*16 + RefCount[32768]*4。
//
// 【关键约束】数组必须落在 exe 镜像基址 ± 2GB 窗口内。
// 理由：DX12 这些站点全都是 "imagebase + RVA" 形态 ——
//   14096e12b  lea r12, cs:140000000h          ; r12 = imagebase
//   14096e147  lea rdi, rva unk_14F30D600[r12] ; = 0x140000000 + 0xF30D600
//   14095f5f3  lea rsi, cs:140000000h
//   14095f62a  mov rax, rva qword_146C7A440[rsi+rcx*8]
// 所以只要把数组放到 imagebase 附近，每个站点【只改 4 字节 disp32】即可，
// 指令本体（含 REX/SIB）完全不动，也不需要 movabs/绝对寻址。
//
// ⚠ 不要分配在 "2GB 以下"：那离 exe 有 5GB，disp32 够不着，反而会逼出绝对寻址。
// ---------------------------------------------------------------------------
// 列表基址解析。.data 段的绝对地址 Loader 校正不了（.pdata 只覆盖函数），
// 所以不能写死；从引用列表的 lea 指令里现取 disp32。指令位置随版本变，序列不变。
// ---------------------------------------------------------------------------
static bool dx12_ResolveListBases() {
    using namespace dx12;

    // 三个 AddRef 实例（IB / SPI / VB）的 lea 之后 16 字节完全相同，只有 disp32 不同，
    // 按地址升序 + index 区分。
    // 该序列在当前 DX12 里共 5 处命中，按地址升序：
    //   1 Texture(0x940762)  2 m_IBList(0x9407B2)  3 m_SPIList(0x940802)
    //   4 Texture(0x940852)  5 m_VBList(0x9408A2)
    // 目标地址已离线核对：index 2/3/5 分别解析出 IB/SPI/VB 列表基址。
    g_ibListVA = (uint64_t)ResolveDataByAob(g_api, g_exeImage, kAob_AddRefList, 2);
    g_spiListVA = (uint64_t)ResolveDataByAob(g_api, g_exeImage, kAob_AddRefList, 3);
    g_vbListVA = (uint64_t)ResolveDataByAob(g_api, g_exeImage, kAob_AddRefList, 5);
    g_computeSpiListVA = (uint64_t)ResolveDataByAob(g_api, g_exeImage, kAob_ComputeSPIList, 1);

    if (!g_ibListVA || !g_vbListVA || !g_spiListVA) {
        g_api->Log("[DX12] 列表基址解析失败 IB=0x%llX VB=0x%llX SPI=0x%llX —— 放弃安装",
            (unsigned long long)g_ibListVA, (unsigned long long)g_vbListVA,
            (unsigned long long)g_spiListVA);
        return false;
    }
    // 自检：三者必须互不相同（同址说明 index 取错了）。
    if (g_ibListVA == g_vbListVA || g_ibListVA == g_spiListVA || g_vbListVA == g_spiListVA) {
        g_api->Log("[DX12] 列表基址自检失败：IB/VB/SPI 出现重复地址");
        return false;
    }

    g_api->Log("[DX12] list bases  IB=0x%llX VB=0x%llX SPI=0x%llX ComputeSPI=0x%llX",
        (unsigned long long)g_ibListVA, (unsigned long long)g_vbListVA,
        (unsigned long long)g_spiListVA, (unsigned long long)g_computeSpiListVA);
    return true;
}

static bool dx12_InitExtendedArrays() {
    using namespace dx12;
    constexpr size_t kRefBytes  = kArrayItems * sizeof(uint32_t);
    constexpr size_t kRecBytes  = kArrayItems * kRecordStride;
    constexpr size_t kExtBytes  = kArrayItems * kExtraStride;
    constexpr size_t kPerList   = kRefBytes + kRecBytes + kExtBytes;
    constexpr size_t kSpiBytes  = kArrayItems * 16 + kArrayItems * sizeof(uint32_t);
    constexpr size_t kTotal     = kPerList * 2 + kSpiBytes;

    // 在 exe 镜像基址附近分配（disp32 可达窗口 = imagebase +- 2GB）。
    // 步长 16MB，优先向下；exe 自身与系统 DLL 占用的区间会被 VirtualAlloc 拒绝。
    const uint64_t exeBase = 0x140000000ULL;
    void* p = nullptr;
    for (int i = 1; i <= 96 && !p; i++) {
        const uint64_t cands[2] = {
            exeBase - 0x1000000ULL * (uint64_t)i,
            exeBase + 0x1000000ULL * (uint64_t)i,
        };
        for (uint64_t cand : cands) {
            if (cand < 0x10000ULL) continue;
            void* q = VirtualAlloc((void*)cand, kTotal, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (q) { p = q; break; }
        }
    }
    if (!p) {
        g_api->Log("[DX12] Failed to allocate extended arrays near imagebase");
        return false;
    }

    // 复核：必须落在相对 imagebase 的有符号 32 位范围内（disp32 可达）
    uint8_t* base = (uint8_t*)p;
    const int64_t delta = (int64_t)base - (int64_t)exeBase;
    if (delta != (int64_t)(int32_t)(uint32_t)delta) {
        g_api->Log("[DX12] arrays 0x%llX outside +-2GB of imagebase (delta 0x%llX)",
            (unsigned long long)(uint64_t)base, (unsigned long long)delta);
        VirtualFree(p, 0, MEM_RELEASE);
        return false;
    }

    g_pIBRefCount = (uint32_t*)base;
    g_pIBRecords  = base + kRefBytes;
    g_pIBExtra    = base + kRefBytes + kRecBytes;
    g_pVBRefCount = (uint32_t*)(base + kPerList);
    g_pVBRecords  = base + kPerList + kRefBytes;
    g_pVBExtra    = base + kPerList + kRefBytes + kRecBytes;
    g_pSPIRefCount = (uint32_t*)(base + kPerList * 2);
    g_pSPIRes      = base + kPerList * 2 + kArrayItems * sizeof(uint32_t);
    // 【诊断】打印每个数组的起止区间，便于把崩溃地址对照到具体数组
    g_api->Log("[DX12] range IB.RefCount [0x%llX,0x%llX)", (unsigned long long)(uint64_t)g_pIBRefCount, (unsigned long long)(uint64_t)((uint8_t*)g_pIBRefCount + kArrayItems*4));
    g_api->Log("[DX12] range IB.records  [0x%llX,0x%llX)", (unsigned long long)(uint64_t)g_pIBRecords, (unsigned long long)(uint64_t)(g_pIBRecords + kArrayItems*kRecordStride));
    g_api->Log("[DX12] range IB.Extra    [0x%llX,0x%llX)", (unsigned long long)(uint64_t)g_pIBExtra, (unsigned long long)(uint64_t)(g_pIBExtra + kArrayItems*kExtraStride));
    g_api->Log("[DX12] range VB.RefCount [0x%llX,0x%llX)", (unsigned long long)(uint64_t)g_pVBRefCount, (unsigned long long)(uint64_t)((uint8_t*)g_pVBRefCount + kArrayItems*4));
    g_api->Log("[DX12] range VB.records  [0x%llX,0x%llX)", (unsigned long long)(uint64_t)g_pVBRecords, (unsigned long long)(uint64_t)(g_pVBRecords + kArrayItems*kRecordStride));
    g_api->Log("[DX12] range VB.Extra    [0x%llX,0x%llX)", (unsigned long long)(uint64_t)g_pVBExtra, (unsigned long long)(uint64_t)(g_pVBExtra + kArrayItems*kExtraStride));
    g_api->Log("[DX12] range SPI.RefCount [0x%llX,0x%llX)", (unsigned long long)(uint64_t)g_pSPIRefCount, (unsigned long long)(uint64_t)((uint8_t*)g_pSPIRefCount + kArrayItems*4));
    g_api->Log("[DX12] range SPI.Resources[0x%llX,0x%llX)", (unsigned long long)(uint64_t)g_pSPIRes, (unsigned long long)(uint64_t)(g_pSPIRes + kArrayItems*16));
    g_api->Log("[DX12] arrays @ 0x%llX (delta 0x%llX) IBrec 0x%llX VBrec 0x%llX SPIres 0x%llX",
        (unsigned long long)(uint64_t)base, (unsigned long long)delta,
        (unsigned long long)(uint64_t)g_pIBRecords,
        (unsigned long long)(uint64_t)g_pVBRecords,
        (unsigned long long)(uint64_t)g_pSPIRes);
    return true;
}

// ===========================================================================
// ===========================================================================
// ===========================================================================
// DX12 重定向：只改 disp32，指令本体不动。
// ===========================================================================
//
// 各站点的 disp32 有【两种基准】，但它们都是"到目标的距离"，所以统一用增量法：
//
//     newDisp32 = oldDisp32 + (新目标 - 原目标)
//
// 这在两种基准下都成立：
//
//   ① RIP 相对（20 个站点，mod=00 rm=101）：
//        48 8D 0D <disp32>          disp32 = target - (addr + 7)
//        基准随指令位置变化，增量法自动抵消。
//
//   ② imagebase 相对（8 个站点，mod=10 rm=100，SIB.base=101）：
//        49 8D 9C 24 <disp32>       disp32 = target - imagebase
//        41 8B 84 C5 <disp32>
//        这些函数开头都有 `lea r12/rsi, cs:140000000h`，所以基准是常量。
//
// 用增量法的好处：不需要判断基准是哪个，也不需要重编码 REX/ModRM/SIB。
//
// ⚠ 第一版实现要求 mod==2（只认 ②），把 20 个 ① 站点全部静默跳过，
//   而 6 个上限字节补丁照常生效 —— 阈值提到 16384 但数组仍是 8192，
//   m_RefCount 越界写坏 m_uCurPipe/m_DeletionPipe，DX12 启动即崩。
//   教训：补丁要么全上，要么全不上，不能"部分生效"。
// RVA 解析失败时的 AOB 兜底：从生成表里找同 rva 的站点，用指令级特征码重新定位。
// 命中后加上 delta（调用侧 rva 与指令起点的差）还原到原来的操作位置。
// 表不存在或未命中返回 0，调用方按原有失败路径处理（fail-closed）。
static uint64_t dx12_SiteFallback(uintptr_t rva, const char* name) {
#if DX12_HAS_SITE_AOB
    for (const auto& site : kDx12SiteAobs) {
        if (site.rva != rva) continue;
        void* hit = g_api->ResolveByPattern(g_exeImage, site.aob, kSectionText, 1);
        if (!hit) return 0;
        g_api->Log("[DX12] %s: RVA 0x%X 失效，AOB 定位到 0x%llX", name,
            (unsigned)rva, (unsigned long long)(uint64_t)hit);
        return (uint64_t)hit + site.delta;
    }
#else
    (void)name;
#endif
    return 0;
}

// 返回 true = 补丁已写入；false = 该站点未处理（会打印原因）。
static bool dx12_PatchRvaDisp(uint64_t rva, uint64_t oldTarget, uint64_t newTarget,
                              const char* name) {
    uint64_t addr = ResolveExe(rva, 16);
    if (!addr) addr = dx12_SiteFallback(rva, name);
    if (!addr) {
        g_api->Log("[DX12] RVA 0x%X unavailable (%s)", (unsigned)rva, name);
        return false;
    }

    uint8_t orig[16];
    memcpy(orig, (void*)addr, sizeof(orig));

    // 解析：REX(可选) + opcode + ModRM [+ SIB] + disp32
    int i = 0;
    if ((orig[i] & 0xF0) == 0x40) i++;          // 可选 REX
    const uint8_t op = orig[i];
    if (op != 0x8D && op != 0x8B && op != 0x8A) {
        g_api->Log("[DX12] unsupported opcode %02X at 0x%llX (%s)", op,
            (unsigned long long)addr, name);
        return false;
    }
    i++;
    const uint8_t modrm = orig[i++];
    const uint8_t mod = (uint8_t)((modrm >> 6) & 0x3);
    const uint8_t rm  = (uint8_t)(modrm & 0x7);

    // disp32 的位置：
    //   mod=00 rm=101            -> 紧跟 ModRM（RIP 相对）
    //   mod=10 rm=100 (+SIB)     -> 紧跟 SIB
    int dispOff = -1;
    if (mod == 0x0 && rm == 0x5) {
        dispOff = i;
    } else if (mod == 0x2 && rm == 0x4) {
        dispOff = i + 1;   // 跳过 SIB
    } else if (mod == 0x2 && rm != 0x4) {
        dispOff = i;       // 无 SIB 的 disp32
    } else {
        g_api->Log("[DX12] unhandled modrm mod=%d rm=%d at 0x%llX (%s)", mod, rm,
            (unsigned long long)addr, name);
        return false;
    }

    const int32_t oldDisp = (int32_t)((uint32_t)orig[dispOff]
        | ((uint32_t)orig[dispOff + 1] << 8)
        | ((uint32_t)orig[dispOff + 2] << 16)
        | ((uint32_t)orig[dispOff + 3] << 24));

    // 增量法：基准（无论 RIP 还是 imagebase）在 old/new 之间抵消
    const int64_t delta = (int64_t)newTarget - (int64_t)oldTarget;
    const int64_t newDisp64 = (int64_t)oldDisp + delta;
    if (newDisp64 != (int64_t)(int32_t)(uint32_t)newDisp64) {
        g_api->Log("[DX12] disp32 overflow at 0x%llX (%s): 0x%llX + 0x%llX",
            (unsigned long long)addr, name, (unsigned long long)oldDisp,
            (unsigned long long)delta);
        return false;
    }
    const uint32_t d = (uint32_t)(int32_t)newDisp64;
    std::vector<uint8_t> nb = {
        (uint8_t)(d & 0xFF), (uint8_t)((d >> 8) & 0xFF),
        (uint8_t)((d >> 16) & 0xFF), (uint8_t)((d >> 24) & 0xFF)
    };
    if (!PatchBytes(addr + dispOff, nb)) {
        g_api->Log("[DX12] disp write failed at 0x%llX (%s)", (unsigned long long)addr, name);
        return false;
    }
    g_api->Log("[DX12] OK %s @0x%llX +%d  disp 0x%X -> 0x%X  (mod=%d rm=%d)",
        name, (unsigned long long)(addr + dispOff), dispOff,
        (unsigned)(uint32_t)oldDisp, (unsigned)d, mod, rm);
    return true;
}


// （disp32 形态已由上面的 dx12_PatchRvaDisp 统一处理，无需单独函数。）


// 通用内存位移（disp32）重定向：`lea/mov reg, [base + ... + disp32]`。
// 用于"基址+偏移"形态的站点 —— 这类站点的 disp32 是【相对某个基址寄存器的偏移】，
// 不是绝对地址，所以既不在 xrefs_to 里，insn_query 的 op1 也匹配不到。
// 处理方式：从原值算增量，只改 disp32 的 4 字节，指令长度与寄存器语义都不变。
//
// 支持两种编码：
//   lea r64, [base+disp32]          48 8D 8x <disp32>   (mod=10, rm!=100)
//   mov r32, [base+idx*8+disp32]    8B BC 08 <disp32>   (mod=10, rm=100, SIB)
static bool dx12_PatchMemDisp(uint64_t rva, int64_t delta, const char* name) {
    uint64_t addr = ResolveExe(rva, 16);
    if (!addr) addr = dx12_SiteFallback(rva, name);
    if (!addr) {
        g_api->Log("[DX12] RVA 0x%X unavailable (%s)", (unsigned)rva, name);
        return false;
    }
    uint8_t orig[16];
    memcpy(orig, (void*)addr, sizeof(orig));
    int i = 0;
    if ((orig[i] & 0xF0) == 0x40) i++;          // 可选 REX
    const uint8_t op = orig[i];
    // 允许的 opcode —— 共同点是【都带 ModRM + disp32，且 disp32 紧跟其后】：
    //   0x8D lea r64, [base+disp32]
    //   0x8B mov r64, [base+disp32]
    //   0x8A mov r8,  [base+disp32]
    //   0x8C mov [base+disp32], sreg        （未用，但形态相同）
    //   0x83 /0 add r/m32, imm8            ← SPI refcount 自减
    //   0xFF /0 inc r/m32 / /1 dec         ← SPI refcount 自增
    //   0x39 cmp r/m32, r32                ← sub_1409454D0 的 Shutdown 判零
    //   0x89 mov r/m32, r32                ← sub_1409454D0 的 Shutdown 清零
    //   0x3B cmp r32, r/m32
    //   0x01 add r/m32, r32 / 0x29 sub / 0x31 xor / 0x21 and / 0x09 or
    //   0x87 xchg
    // 注意：0x39/0x89 的 ModRM.reg 是【寄存器】，不像 0x83/0xFF 那样是 opcode 扩展位，
    //       所以不能对它们套用 "(modrm & 0x38) == 0" 的判据。
    static const uint8_t kSimpleOps[] = { 0x8D, 0x8B, 0x8A, 0x8C, 0x39, 0x89, 0x3B, 0x01, 0x29, 0x31, 0x21, 0x09, 0x87 };
    bool opAllowed = false;
    for (uint8_t o : kSimpleOps) if (op == o) { opAllowed = true; break; }
    const bool isGroup1 = (op == 0x83 && (orig[i + 1] & 0x38) == 0x00);
    const bool isIncDec = (op == 0xFF && (orig[i + 1] & 0x38) == 0x00);
    if (!opAllowed && !isGroup1 && !isIncDec) {
        g_api->Log("[DX12] memdisp: unsupported opcode %02X at 0x%llX (%s)", op,
            (unsigned long long)addr, name);
        return false;
    }
    i++;
    const uint8_t modrm = orig[i++];
    if (((modrm >> 6) & 0x3) != 0x2) {
        g_api->Log("[DX12] memdisp: not disp32 at 0x%llX (%s)", (unsigned long long)addr, name);
        return false;
    }
    if ((modrm & 0x7) == 0x4) i++;              // 跳过 SIB
    const int dispOff = i;
    const int32_t oldDisp = (int32_t)((uint32_t)orig[dispOff]
        | ((uint32_t)orig[dispOff + 1] << 8)
        | ((uint32_t)orig[dispOff + 2] << 16)
        | ((uint32_t)orig[dispOff + 3] << 24));
    const int64_t newDisp = (int64_t)oldDisp + delta;
    if (newDisp != (int64_t)(int32_t)(uint32_t)newDisp) {
        g_api->Log("[DX12] memdisp overflow at 0x%llX (%s): 0x%X + 0x%llX",
            (unsigned long long)addr, name, (unsigned)oldDisp, (unsigned long long)delta);
        return false;
    }
    const uint32_t d = (uint32_t)(int32_t)newDisp;
    std::vector<uint8_t> nb = {
        (uint8_t)(d & 0xFF), (uint8_t)((d >> 8) & 0xFF),
        (uint8_t)((d >> 16) & 0xFF), (uint8_t)((d >> 24) & 0xFF)
    };
    if (!PatchBytes(addr + dispOff, nb)) {
        g_api->Log("[DX12] memdisp write failed at 0x%llX (%s)", (unsigned long long)addr, name);
        return false;
    }
    g_api->Log("[DX12] OK %s @0x%llX +%d  disp 0x%X -> 0x%X", name,
        (unsigned long long)(addr + dispOff), dispOff, (unsigned)(uint32_t)oldDisp, (unsigned)d);
    return true;
}

// "add r64, imm32" 形态的重定向（48 81 C1 <imm32>）。
// 用于 sub_14094A470 / sub_14094A4D0 里的 `add rcx, 8400h`：
// 该处 rcx 已经是 m_IBList + 5*idx*8，再 +0x8400 得到原始记录数组元素。
// 把 imm32 改成 (新记录数组 - 列表基址) 即可让它指向插件数组里的同一个元素。
// imm32 是 sign-extended，负值（减法）也合法。
static bool dx12_PatchAddImm32(uint64_t rva, int64_t newImm, const char* name) {
    uint64_t addr = ResolveExe(rva, 16);
    if (!addr) addr = dx12_SiteFallback(rva, name);
    if (!addr) {
        g_api->Log("[DX12] RVA 0x%X unavailable (%s)", (unsigned)rva, name);
        return false;
    }
    if (newImm != (int64_t)(int32_t)(uint32_t)newImm) {
        g_api->Log("[DX12] imm32 out of range at 0x%llX (%s): 0x%llX",
            (unsigned long long)addr, name, (unsigned long long)newImm);
        return false;
    }
    uint8_t orig[8];
    memcpy(orig, (void*)addr, sizeof(orig));
    // 期望 48 81 C1 <imm32>（REX.W + 81 /0 + ModRM C1）
    if (!(orig[0] == 0x48 && orig[1] == 0x81 && (orig[2] & 0xF8) == 0xC0)) {
        g_api->Log("[DX12] unexpected add bytes at 0x%llX (%s): %02X %02X %02X",
            (unsigned long long)addr, name, orig[0], orig[1], orig[2]);
        return false;
    }
    const int32_t oldImm = (int32_t)((uint32_t)orig[3] | ((uint32_t)orig[4] << 8)
        | ((uint32_t)orig[5] << 16) | ((uint32_t)orig[6] << 24));
    const uint32_t d = (uint32_t)(int32_t)newImm;
    std::vector<uint8_t> nb = {
        (uint8_t)(d & 0xFF), (uint8_t)((d >> 8) & 0xFF),
        (uint8_t)((d >> 16) & 0xFF), (uint8_t)((d >> 24) & 0xFF)
    };
    if (!PatchBytes(addr + 3, nb)) {
        g_api->Log("[DX12] add-imm write failed at 0x%llX (%s)", (unsigned long long)addr, name);
        return false;
    }
    g_api->Log("[DX12] OK %s @0x%llX  add imm 0x%X -> 0x%X", name,
        (unsigned long long)addr, (unsigned)(uint32_t)oldImm, (unsigned)d);
    return true;
}

// ---------------------------------------------------------------------------
// Reset 的 m_RefCount memset 长度补丁。
// 形态：41 B8 <imm32>   (= mov r8d, imm32；随后 call Platform::memset)
//
// 为什么必须改：Reset 把 m_cItems 置 1 后清引用计数数组，但清的长度是
// 【旧容量】的字节数（IB/VB 0x8000 = 8192 项，SPI 0x10000 = 16384 项）。
// 而 lea 已经被重定向到 65536 项的插件数组 —— 只清前 8192/16384 项，
// 剩余槽位的旧计数会留到下一局。AddResource 是靠扫非 0 槽找空位的，
// 这些残留会被当成"仍在使用"，表现为句柄泄漏 / 索引一路漂高。
// ---------------------------------------------------------------------------
static bool dx12_PatchMovImm32(uint64_t rva, uint32_t expectImm, uint32_t newImm,
                               const char* name) {
    uint64_t addr = ResolveExe(rva, 16);
    if (!addr) addr = dx12_SiteFallback(rva, name);
    if (!addr) {
        g_api->Log("[DX12] RVA 0x%X unavailable (%s)", (unsigned)rva, name);
        return false;
    }
    uint8_t orig[6];
    memcpy(orig, (void*)addr, sizeof(orig));
    if (!(orig[0] == 0x41 && orig[1] == 0xB8)) {
        g_api->Log("[DX12] unexpected mov r8d bytes at 0x%llX (%s): %02X %02X",
            (unsigned long long)addr, name, orig[0], orig[1]);
        return false;
    }
    const uint32_t oldImm = (uint32_t)orig[2] | ((uint32_t)orig[3] << 8)
        | ((uint32_t)orig[4] << 16) | ((uint32_t)orig[5] << 24);
    if (oldImm != expectImm) {
        g_api->Log("[DX12] unexpected mov r8d imm at 0x%llX (%s): 0x%X (expect 0x%X)",
            (unsigned long long)addr, name, (unsigned)oldImm, (unsigned)expectImm);
        return false;
    }
    std::vector<uint8_t> nb = {
        0x41, 0xB8,
        (uint8_t)(newImm & 0xFF), (uint8_t)((newImm >> 8) & 0xFF),
        (uint8_t)((newImm >> 16) & 0xFF), (uint8_t)((newImm >> 24) & 0xFF)
    };
    if (!PatchBytes(addr, nb)) {
        g_api->Log("[DX12] mov r8d write failed at 0x%llX (%s)", (unsigned long long)addr, name);
        return false;
    }
    g_api->Log("[DX12] OK %s @0x%llX  memset 0x%X -> 0x%X", name,
        (unsigned long long)addr, (unsigned)oldImm, (unsigned)newImm);
    return true;
}

// ---------------------------------------------------------------------------
// 【整条指令替换】用 asmjit 重新生成一条语义等价的指令，只把绝对地址换成新值。
// 相比"改 disp32"的手法，它不需要心算增量、不会漏减指令里固有的偏移，
// 也不可能把 disp32 算错 —— 因为地址是【直接写进去的】。
//
// 约束：新编码长度必须 == 原指令长度（调用方保证）。多出的字节用 NOP 填充。
// 之所以能等长，是因为这些站点的 disp32 原值都很小（0x80 / 0x58540 / 0x8400），
// 绝对寻址后仍是 32 位（mod=10），指令形态与长度都不变。
// ---------------------------------------------------------------------------
// 原指令的形态（从 exe 原始字节解析得出，见注释里的字节）
// 覆盖 3 种：
//   A) [rbx + idx*4 + disp32]       —— SPI refcount（inc / add / mov）
//   B) [rsi + idx*4 + disp32]       —— SPI Shutdown（cmp / mov）
//   C) [base + disp32]（无 index）  —— IB/VB Release 的 lea/mov
//   D) add r64, imm32               —— IB/VB Release 的 add rcx,8400h
//
// 共同点：disp32 原值都很小（0x80 / 0x58540 / 0x8400），
// 换成"新数组绝对地址"后仍然是 32 位可表示（< 0x100000000），
// 且 ModRM 保持 mod=10，所以【指令长度不变】，可以原地等长替换。
//
// ⚠ 不能对 [base + disp32] 里 base 是 imagebase 的站点用这招 ——
//   那时新地址可能超过 32 位。本函数只处理 disp32 语义明确的站点。
static bool dx12_ReplaceInsn(uintptr_t rva, const std::vector<uint8_t>& newBytes,
                             const char* name) {
    uint64_t addr = ResolveExe(rva, 16);
    if (!addr) addr = dx12_SiteFallback(rva, name);
    if (!addr) {
        g_api->Log("[DX12] replace: RVA 0x%X unavailable (%s)", (unsigned)rva, name);
        return false;
    }
    // 读原始长度（用 Zydis 解码，避免手写长度表出错）
    uint64_t origLen = 0;
    if (!DecodeInsn(addr, &origLen, nullptr)) {
        g_api->Log("[DX12] replace: decode failed at 0x%llX (%s)", (unsigned long long)addr, name);
        return false;
    }
    if (newBytes.size() > origLen) {
        g_api->Log("[DX12] replace: new insn %zu bytes > original %llu at 0x%llX (%s)",
            newBytes.size(), (unsigned long long)origLen, (unsigned long long)addr, name);
        return false;
    }
    if (newBytes.size() != origLen) {
        // 长度不等 = 编码形态与原指令不一致（最典型：寄存器传错，少了 REX.B）。
        // 这种"能编出来但语义不同"的错误必须显式暴露，不能靠 NOP 掩盖。
        g_api->Log("[DX12] replace: ★长度不等 %zu != %llu @0x%llX (%s) —— 编码形态可疑，已跳过",
            newBytes.size(), (unsigned long long)origLen, (unsigned long long)addr, name);
        return false;
    }
    std::vector<uint8_t> buf = newBytes;
    if (!PatchBytes(addr, buf)) {
        g_api->Log("[DX12] replace: write failed at 0x%llX (%s)", (unsigned long long)addr, name);
        return false;
    }
    g_api->Log("[DX12] replace %s @0x%llX  %zu/%llu 字节  ->  0x%llX",
        name, (unsigned long long)addr, newBytes.size(), (unsigned long long)origLen,
        (unsigned long long)rva);
    return true;
}

// 用 asmjit 生成一条 "[base + idx*scale + disp32]" 指令。
//   kind: 0=inc dword, 1=add dword -1, 2=mov esi,[..], 3=cmp [..],ebp, 4=mov [..],ebp
//   base: 基址寄存器（rbx / rsi）
//   idx : 索引寄存器（rcx / r8 / rax），nullptr 表示无索引
//   newDisp: 要写入的 disp32（= 新数组绝对地址 - 该寄存器运行时持有的基址）
static bool dx12_AsmMemInsn(int kind, asmjit::x86::Gp base, asmjit::x86::Gp idx,
                            int32_t newDisp, std::vector<uint8_t>* out, const char* name) {
    using namespace asmjit;
    CodeHolder code;
    code.init(rt.environment());
    x86::Assembler a(&code);
    const auto m = x86::dword_ptr(base, idx, 2, newDisp);   // scale=2（*4），disp32
    // asmjit 1.17 没有 CodeHolder::error()，每条 emit 返回 Error，逐条检查。
    Error err = kErrorOk;
    switch (kind) {
    case 0: err = a.inc(m); break;
    case 1: err = a.add(m, -1); break;
    case 2: err = a.mov(x86::esi, m); break;
    case 3: err = a.cmp(m, x86::ebp); break;
    case 4: err = a.mov(m, x86::ebp); break;
    default:
        g_api->Log("[DX12] asm: bad kind %d (%s)", kind, name);
        return false;
    }
    if (err != kErrorOk) {
        g_api->Log("[DX12] asm: emit error %s (%s)", DebugUtils::errorAsString(err), name);
        return false;
    }
    out->assign(code.textSection()->buffer().data(),
                code.textSection()->buffer().data() + code.textSection()->buffer().size());
    return true;
}

// 上限字节补丁
static void dx12_PatchLimitByte(uintptr_t rva, uint8_t byte, const char* name) {
    uint64_t addr = ResolveExe(rva, 4);
    if (!addr) addr = dx12_SiteFallback(rva, name);
    if (!addr) {
        g_api->Log("[DX12] RVA 0x%X unavailable (%s)", (unsigned)rva, name);
        return;
    }
    if (!PatchBytes(addr, std::vector<uint8_t>{ byte })) {
        g_api->Log("[DX12] limit patch failed at 0x%llX (%s)", (unsigned long long)addr, name);
        return;
    }
    g_api->Log("[DX12] limit 0x%llX = 0x%02X  %s", (unsigned long long)addr, byte, name);
}

// DX12：安装 4 个 hook。与 DX11 版的差别只有地址与"this 基址常量"：
//   - IB/VB 的 AddResource 仍共用一个实体（CreateIB / CreateVB 都 call 0x940BE0）
//   - IB/VB 的 ReleaseResource 也共用一个实体（两个 15 字节 thunk 传不同基址给 0x94D670）
//   - SPI 各自独立
// 因此 hook 体的汇编可以逐字复用 DX11 版（两边站点字节完全相同）。
static bool dx12_InstallHooks() {
    using namespace dx12;
    const uint64_t mVBList = g_vbListVA;
    const uint64_t mComputeSPIList = g_computeSpiListVA;
    if (!mVBList || !mComputeSPIList) {
        g_api->Log("[DX12] list base resolution failed");
        return false;
    }

    int ok = 0;

    // ---- SPI AddResource / ReleaseResource：已改为 disp32 原地改写 ----
    // 不再需要 hook：DX12 的 m_SPIList 与 m_ComputeSPIList 各有独立的
    // AddResource/ReleaseResource 函数（sub_140941270 的 prologue 是 mov rbx,rcx，
    // 且只有一个调用点），rbx 恒为 m_SPIList，disp32 可直接改写。
    // 三条指令的重定向在 dx12_ApplyRedirectPatches() 里完成。

    // ---- IB/VB AddResource ----
// 【分流模式】index < ORIG_LIMIT 时跳回原函数（写【旧】数组），
// index >= ORIG_LIMIT 时写插件的新数组，然后跳到 0x940D18。
//
// 与 DX11 的对应关系（DX11 hookAddr=0x9408A1, firstSize=单条指令长度）：
//   < LIMIT  -> trampoline+0          （执行原 inc，写旧数组，再跳 0x940D18）
//   >= LIMIT -> 自己 inc 新数组，跳 0x940D18
// DX12 的跳回点是绝对地址 0x940D18（= hookAddr+7，紧接 inc 之后），
// 所以 >= LIMIT 分支不需要 firstSize，直接跳绝对地址即可。
//
// 【不分流】index < ORIG_LIMIT 时也写【新数组】。
// 为什么必须一致：m_RefCount / records / ExtraData 都已被【无条件】重定向到新数组，
// 若 hook 在 index < ORIG_LIMIT 时仍写旧数组，同一个索引的 refcount 会分裂在
// 两个数组里 —— AddRef 加新数组、Release 减旧数组，旧数组恒 0 且被减成 0xFFFFFFFF，
// 于是回收队列在错误时机被填充 -> 句柄被重复分配 -> GPU 侧 use-after-free。
// 注意：区分【列表】(IB vs VB) 的 cmp rbx, mVBList 仍必须保留，那是另一回事。
#define DX12_IBVB_SPLIT 0
    {
        // 站点解析：RVA 主路径 + AOB 兜底 + 入口字节校验（docs/PLUGIN_GUIDE.md §5.2）
        static constexpr HookInfo kSiteAdd = HookRva(
            0x940D11, "FF 84 8B 80 00 00 00 89 73 40 49 8B 5B 10 49 8B 73 18");
        static const uint8_t kExpectAdd[] = { 0xFF, 0x84, 0x8B, 0x80, 0x00, 0x00, 0x00 };
        const uint64_t hookAddr = ResolveSite(kSiteAdd, kExpectAdd, sizeof(kExpectAdd), "DX12/AddResource");
        const uint64_t jumpAfterInc = hookAddr ? hookAddr + 7 : 0;   // inc 之后 = mov [rbx+0x40],esi
        if (hookAddr && jumpAfterInc) {
            using namespace asmjit;
            CodeHolder code;
            code.init(rt.environment());
            x86::Assembler a(&code);
            x86::Gp indexReg = x86::rcx;
            x86::Gp tempReg = x86::r9;
            x86::Gp listReg = x86::r10;
            Label lVB = a.newLabel();
#if DX12_IBVB_SPLIT
            Label lOrig = a.newLabel();
#endif

            ASMJIT_EMIT(a.push(tempReg));
            ASMJIT_EMIT(a.push(listReg));
#if DX12_IBVB_SPLIT
            // 分流判据用【无符号】比较：index 是 u16 零扩展，恒为非负。
            ASMJIT_EMIT(a.cmp(indexReg, (uint64_t)ORIG_LIMIT));
            ASMJIT_EMIT(a.jb(lOrig));
#endif
            // this == m_VBList ? VB 数组 : IB 数组
            ASMJIT_EMIT(a.mov(listReg, (uint64_t)mVBList));
            ASMJIT_EMIT(a.cmp(x86::rbx, listReg));
            ASMJIT_EMIT(a.je(lVB));
            ASMJIT_EMIT(a.mov(tempReg, (uint64_t)g_pIBRefCount));
            ASMJIT_EMIT(a.inc(x86::dword_ptr(tempReg, indexReg, 2)));
            ASMJIT_EMIT(a.pop(listReg));
            ASMJIT_EMIT(a.mov(tempReg, (uint64_t)jumpAfterInc));
            ASMJIT_EMIT(a.xchg(tempReg, x86::ptr(x86::rsp)));
            ASMJIT_EMIT(a.ret());
            a.bind(lVB);
            ASMJIT_EMIT(a.mov(tempReg, (uint64_t)g_pVBRefCount));
            ASMJIT_EMIT(a.inc(x86::dword_ptr(tempReg, indexReg, 2)));
            ASMJIT_EMIT(a.pop(listReg));
            ASMJIT_EMIT(a.mov(tempReg, (uint64_t)jumpAfterInc));
            ASMJIT_EMIT(a.xchg(tempReg, x86::ptr(x86::rsp)));
            ASMJIT_EMIT(a.ret());
#if DX12_IBVB_SPLIT
            // < LIMIT：跳回 trampoline 起点，由它执行原始 inc（写旧数组）后跳到 0x940D18。
            // 用 trampoline 而非硬编码地址，避免与 InstallHook 的改写长度假设耦合。
            a.bind(lOrig);
            ASMJIT_EMIT(a.mov(tempReg, (uint64_t)&g_origAddResource));
            ASMJIT_EMIT(a.mov(tempReg, x86::qword_ptr(tempReg)));
            ASMJIT_EMIT(a.pop(listReg));
            ASMJIT_EMIT(a.xchg(tempReg, x86::ptr(x86::rsp)));
            ASMJIT_EMIT(a.ret());
#endif
            if (FinalizeAndInstall(hookAddr, &g_origAddResource, "DX12/AddResource", code, 2)) ok++;
        }
    }

// ---- IB/VB ReleaseResource ----
// 【分流模式】index < ORIG_LIMIT 时跳回 trampoline（执行原 add+mov，写旧数组），
// index >= ORIG_LIMIT 时在新数组上做 add -1 + 读回，然后跳到 0x94D6C2。
//
// 指令布局（已用字节验证）：
//   0x94D6B1  42 83 84 83 80 00 00 00 ff   add dword [rbx+r8*4+80h], -1   (9B)
//   0x94D6BA  42 8b b4 83 80 00 00 00      mov esi, [rbx+r8*4+80h]        (8B)
//   0x94D6C2  75 5F                        jnz  -> 依赖 add 产生的 ZF
// 跳回点 0x94D6C2 = hookAddr+17（两条指令都跳过，因为新数组上已经做完）。
// trampoline 内含 add+mov 两条原始指令，所以 < LIMIT 分支跳它起点即可，
// ZF 由 trampoline 里的 add 产生，jnz 语义正确。
    {
        // 站点解析：RVA 主路径 + AOB 兜底 + 入口字节校验（docs/PLUGIN_GUIDE.md §5.2）
        static constexpr HookInfo kSiteRel = HookRva(
            0x94D6B1, "42 83 84 83 80 00 00 00 FF 42 8B B4 83 80 00 00 00 75 5F");
        static const uint8_t kExpectRel[] = { 0x42, 0x83, 0x84, 0x83, 0x80, 0x00, 0x00, 0x00, 0xFF };
        const uint64_t hookAddr = ResolveSite(kSiteRel, kExpectRel, sizeof(kExpectRel), "DX12/Release");
        const uint64_t jumpAfterRead = hookAddr ? hookAddr + 17 : 0;  // add+mov 之后 = jnz
        if (hookAddr && jumpAfterRead) {
            using namespace asmjit;
            CodeHolder code;
            code.init(rt.environment());
            x86::Assembler a(&code);
            x86::Gp indexReg = x86::r8;
            x86::Gp tempReg = x86::r9;
            x86::Gp listReg = x86::r10;
            Label lVB = a.newLabel();
#if DX12_IBVB_SPLIT
            Label lOrig = a.newLabel();
#endif

            ASMJIT_EMIT(a.push(tempReg));
            ASMJIT_EMIT(a.push(listReg));
#if DX12_IBVB_SPLIT
            ASMJIT_EMIT(a.cmp(indexReg, (uint64_t)ORIG_LIMIT));
            ASMJIT_EMIT(a.jb(lOrig));
#endif
            ASMJIT_EMIT(a.mov(listReg, (uint64_t)mVBList));
            ASMJIT_EMIT(a.cmp(x86::rbx, listReg));
            ASMJIT_EMIT(a.je(lVB));
            ASMJIT_EMIT(a.mov(tempReg, (uint64_t)g_pIBRefCount));
            ASMJIT_EMIT(a.add(x86::dword_ptr(tempReg, indexReg, 2), -1));   // 自减
            ASMJIT_EMIT(a.mov(x86::esi, x86::ptr(tempReg, indexReg, 2)));   // 读回（原指令语义）
            // 只 pop r10；r9 留在栈上由后面的 xchg+ret 抵消（入口 push 两次）。
            ASMJIT_EMIT(a.pop(listReg));
            ASMJIT_EMIT(a.mov(tempReg, (uint64_t)jumpAfterRead));
            ASMJIT_EMIT(a.xchg(tempReg, x86::ptr(x86::rsp)));
            ASMJIT_EMIT(a.ret());
            a.bind(lVB);
            ASMJIT_EMIT(a.mov(tempReg, (uint64_t)g_pVBRefCount));
            ASMJIT_EMIT(a.add(x86::dword_ptr(tempReg, indexReg, 2), -1));
            ASMJIT_EMIT(a.mov(x86::esi, x86::ptr(tempReg, indexReg, 2)));
            ASMJIT_EMIT(a.pop(listReg));
            ASMJIT_EMIT(a.mov(tempReg, (uint64_t)jumpAfterRead));
            ASMJIT_EMIT(a.xchg(tempReg, x86::ptr(x86::rsp)));
            ASMJIT_EMIT(a.ret());
#if DX12_IBVB_SPLIT
            a.bind(lOrig);
            ASMJIT_EMIT(a.mov(tempReg, (uint64_t)&g_origRelease));
            ASMJIT_EMIT(a.mov(tempReg, x86::qword_ptr(tempReg)));
            ASMJIT_EMIT(a.pop(listReg));
            ASMJIT_EMIT(a.xchg(tempReg, x86::ptr(x86::rsp)));
            ASMJIT_EMIT(a.ret());
#endif
            if (FinalizeAndInstall(hookAddr, &g_origRelease, "DX12/Release", code, 3)) ok++;
        }
    }

    g_api->Log("[DX12] hooks installed: %d/2", ok);
    return ok > 0;
}

// ---------------------------------------------------------------------------
// Shutdown 全表清理 sub_140945160 的修复（DX12 退出必崩的根因）
// ---------------------------------------------------------------------------
// 【函数原始形态】0x140945160，size 0xAE，IDA 名为 IBVB_Shutdown：
//
//   140945160  mov  [rsp+arg_10], rbp
//   140945165  mov  [rsp+arg_18], rsi
//   14094516a  push rdi
//   14094516b  sub  rsp, 20h
//   14094516f  xor  ebp, ebp
//   140945171  mov  rsi, rcx                 ; rsi = 列表基址（唯一参数）
//   140945174  movzx edi, bp
//   140945177  mov  [rsp+arg_0], bp          ; i（u16，就地保存）
//   14094517c  cmp  [rcx], ebp               ; m_cItems
//   14094517e  jbe  1FE
//   140945180  mov  [rsp+arg_8], rbx
//   140945190  movzx eax, di                 ; idx = (u32)i
//   140945193  cmp  [rsi+rax*4+80h], ebp     ; ← 读 m_RefCount[idx]（硬编码）
//   14094519a  jz   1EA
//   14094519c  lea  rbx, [rsi+58400h]        ; &m_DeleteList
//   1409451a3  mov  [rsi+rax*4+80h], ebp     ; ← 清零
//   1409451aa  mov  eax, [rbx+120h]          ; nCount
//   1409451b0  mov  edx, [rbx+118h]          ; nCapacity
//   1409451b6  cmp  eax, edx
//   1409451b8  jnz  1CE
//   1409451ba  inc  edx                      ; nCapacity+1
//   1409451bc  lea  r8, [rsp+28h+arg_0]      ; &i
//   1409451c1  xor  r9d, r9d
//   1409451c4  mov  rcx, rbx
//   1409451c7  call Vector::RequestAndAppendLinearBlock   ; 扩容并 append
//   1409451cc  jmp  1E4
//   1409451ce  mov  rcx, rax
//   1409451d1  mov  rax, [rbx+110h]          ; pStorage
//   1409451d8  lea  rdx, [rax+rcx*2]
//   1409451dc  test rdx, rdx
//   1409451df  jz   1E4
//   1409451e1  mov  [rdx], di                ; append(i)
//   1409451e4  inc  dword ptr [rbx+120h]     ; ++nCount
//   1409451ea  inc  di                       ; ++i
//   1409451ed  movzx eax, di
//   1409451f0  mov  [rsp+28h+arg_0], di
//   1409451f5  cmp  eax, [rsi]
//   1409451f7  jb   190
//   1409451f9  mov  rbx, [rsp+arg_8]
//   1409451fe  mov  rbp, [rsp+arg_10]
//   140945203  mov  rsi, [rsp+arg_18]
//   140945208  add  rsp, 20h
//   14094520c  pop  rdi
//   14094520d  retn
//
// 【为什么必须整体替换，不能只改 rcx】
//   rsi 同时承担三个用途：m_cItems([rsi])、m_DeleteList([rsi+0x58400])、
//   m_RefCount([rsi+rax*4+0x80])。任何"把基址换成伪基址"的做法都会让
//   m_cItems 与 m_DeleteList 一起错位 —— 崩溃只是被挪到别的地方。
//
// 【为什么不能打 disp32 补丁】
//   本函数只有一份函数体，却被 Renderer::Shutdown 用两个不同的 rcx 调用：
//     0x950C61  rcx = &m_VBList
//     0x950C6D  rcx = &m_IBList
//   而 (g_pVBRefCount - m_VBList) != (g_pIBRefCount - m_IBList)，
//   一条指令里只有一个 disp32，写死任一个都会让另一个列表读错内存。
//
// 【崩溃机理】（每一环都用 IDA 与诊断日志核对过）
//   m_RefCount 起点 = 列表基址+0x80，8192 项，恰好结束于 +0x8080；
//   而 +0x8080 正是 m_uCurPipe，+0x8088 起是 m_DeletionPipe[0..2]（每项 0x128）。
//   Reset(0x971490) 的 memset 长度仍是旧容量 0x8000(=8192*4)，
//   于是 idx>=8192 时读到的不是 refcount，而是【活跃的 pipe 结构】：
//     idx 8192 -> +0x8080 m_uCurPipe      (0/1/2，非零)
//     idx 8344 -> +0x82E0 pipe[2] 内部    (指针高位，必非零)
//     idx 8350 -> +0x82F8 pipe[2] 内部    (必非零)
//   这些值被当成句柄推进 m_DeleteList，Reap(0x96E100) 再用它索引 ExtraData，
//   拿到从未初始化过的槽位（诊断实测 slotNonZeroDwords=0/28），
//   f50==0 -> sub_140970250 解引用 [0+0x40] -> 0xC0000005。
//   这也解释了【只有退出崩】：本函数只在 Shutdown 里被调用。
//
// 【hook 方案】入口按 rcx 判定 IB / VB，选对应新数组，然后执行一份
// 与原始函数【语义逐条等价】的 C++ 实现。原函数体不再执行，因此
// "被覆盖的指令"就是整个函数 —— 下面这份实现已经把它们全部覆盖。
// 注意：这里【刻意不保存】MinHook 给出的 trampoline。
// 整函数替换的 hook 一旦回调 trampoline，就会把原始函数体整段跑一遍，
// 让被替换掉的缺陷重新生效。本修复完全自包含，不需要 original。

// m_DeleteList 的布局（已用 Reap/Reset/sub_14096F4F0 三处交叉验证）：
//   &m_DeleteList      = 列表基址 + 0x58400
//   pStorage           = +0x58400 + 0x110 = 列表基址 + 0x58510
//   nCapacity          = +0x58400 + 0x118 = 列表基址 + 0x58518
//   nCount             = +0x58400 + 0x120 = 列表基址 + 0x58520
static constexpr uint32_t kDelVecOff   = 0x58400;
static constexpr uint32_t kDelPStorage = 0x110;
static constexpr uint32_t kDelNCap     = 0x118;
static constexpr uint32_t kDelNCount   = 0x120;

// 说明：原函数在 0x9451C7 调 Vector::RequestAndAppendLinearBlock(0x14094F070) 扩容。
// 这里不再调用它 —— 扩容已在本文件内自实现（见下方 kInlineCap 那段），
// 分配器与引擎析构器使用的 CRT aligned_malloc/aligned_free 同源。
// 因此本修复对引擎只保留【1 个 RVA 依赖】：hook 目标 0x945160。

static void __fastcall Dx12ShutdownCleanupHook(void* listPtr) {
    using namespace dx12;
    uint8_t* const base = (uint8_t*)listPtr;

    // ---- 1) 分流：按列表基址选新 refcount 数组 ----
    //
    // ⚠ 这里【绝对不能】回落到 g_pShutdownCleanupOrig（MinHook trampoline）。
    //   本 hook 是"整函数替换"：trampoline 里只有原函数开头被搬走的那几条指令，
    //   执行完会跳回 hookAddr+N 继续跑【原始函数体】，
    //   于是 cmp/mov [rsi+rax*4+80h] 照样读旧数组 —— 修复会被静默绕过。
    //   所以未知列表也只能用自包含逻辑处理：退回列表自身的原地数组
    //   （base+0x80），这正是原函数的行为，只是不再有越界读 pipe 的缺陷。
    uint32_t* ref = nullptr;
    bool usingInPlace = false;
    if ((uintptr_t)base == (uintptr_t)g_vbListVA)      ref = g_pVBRefCount;
    else if ((uintptr_t)base == (uintptr_t)g_ibListVA) ref = g_pIBRefCount;
    else {
        // 理论上不会发生（该函数只有 Shutdown 里那两个调用点）。
        ref = (uint32_t*)(base + 0x80);
        usingInPlace = true;
        g_api->Log("[DX12] IBVB/Shutdown: unexpected list 0x%llX -> in-place refcount",
            (unsigned long long)(uintptr_t)listPtr);
    }

    // ---- 2) 取三个字段（与原函数 [rsi] / [rsi+0x58400+...] 一一对应）----
    //
    // 【上界钳制】原函数直接信 m_cItems，而插件把上限提到了 16384/32768。
    // 新数组容量是 kArrayItems(65536)，正常不会超；但万一 m_cItems 被写脏，
    // 不钳制就会越界读——那正是我们要消灭的故障形态。这里加一道硬闸。
    uint32_t nItems = *(const uint32_t*)(base + 0x00);                // m_cItems
    if (nItems > (uint32_t)dx12::kArrayItems) {
        g_api->Log("[DX12] IBVB/Shutdown: m_cItems=%u > capacity %u, clamped",
            nItems, (unsigned)dx12::kArrayItems);
        nItems = (uint32_t)dx12::kArrayItems;
    }
    if (usingInPlace && nItems > (uint32_t)ORIG_LIMIT) {
        // 原地数组只有 ORIG_LIMIT(8192) 项，超出部分同样是越界读
        nItems = (uint32_t)ORIG_LIMIT;
    }
    uint8_t* const vec    = base + kDelVecOff;                        // &m_DeleteList
    uint16_t** const pSto = (uint16_t**)(vec + kDelPStorage);
    uint32_t* const nCap  = (uint32_t*)(vec + kDelNCap);
    uint32_t* const nCnt  = (uint32_t*)(vec + kDelNCount);

    // ---- 3) 自实现扩容：不再依赖引擎的 Vector::RequestAndAppendLinearBlock ----
    //
    // m_DeleteList 是 Types::Vector<u16, Threshold<FixedInline<u16,128>, Default<8,25>,128>>：
    //   pStorage  @ vec+0x110
    //   nCapacity @ vec+0x118
    //   nCount    @ vec+0x120
    // 容量 <= 0x80(128) 时 pStorage 指向结构体内的内联缓冲；> 0x80 时指向堆块。
    //
    // 引擎侧用的就是 CRT 那一套，没有任何私有分配器：
    //   Platform::Alloc(0x98FF60)  == aligned_malloc((bytes+15)&~15, max(16,align))
    //   析构器(0x140F99EB0)        == 当 nCapacity > 0x80 时 aligned_free(pStorage)
    // 二者底层都是 HeapAlloc/HeapFree(GetProcessHeap())。本 detour 就在游戏进程、
    // 游戏线程里执行，插件（/MT 静态 CRT）的 _aligned_malloc 同样落在进程堆上，
    // 所以"插件分配、引擎释放"是匹配的 —— 不需要再借引擎的分配函数。
    constexpr uint32_t kInlineCap = 0x80;
    {
        const uint32_t need = *nCnt + nItems;
        // pStorage 为空说明这个 vector 从未初始化过：即便 need 很小也必须走堆分配，
        // 否则内联分支会保留空指针，后面的 append 就是往 0 写。
        if (need > *nCap || *pSto == nullptr) {
            if (need <= kInlineCap && *pSto) {
                // 内联缓冲本来就能装下，只需把容量记账推上去，不动 pStorage
                *nCap = need;
            } else {
                const size_t bytes = (size_t)need * sizeof(uint16_t);
                void* nb = _aligned_malloc((bytes + 15u) & ~(size_t)15u, 16);
                if (!nb) {
                    g_api->Log("[DX12] IBVB/Shutdown: grow %u->%u failed (cap)",
                        *nCap, need);
                } else {
                    if (*nCnt && *pSto)
                        memcpy(nb, *pSto, (size_t)(*nCnt) * sizeof(uint16_t));
                    // 旧块只有在容量超过内联阈值时才是堆块；否则 pStorage 指向结构体内部
                    if (*nCap > kInlineCap && *pSto) _aligned_free(*pSto);
                    *pSto = (uint16_t*)nb;
                    *nCap = need;
                }
            }
        }
    }

    for (uint32_t i = 0; i < nItems; ++i) {
        if (ref[i] == 0) continue;          // cmp [..], ebp / jz
        ref[i] = 0;                         // mov [..], ebp
        const uint32_t cnt = *nCnt;
        if (cnt >= *nCap) continue;         // 硬闸：绝不越界写
        uint16_t* slot = (uint16_t*)((uint8_t*)*pSto + (uint64_t)cnt * 2);
        if (slot) *slot = (uint16_t)i;      // append(i)
        ++(*nCnt);
    }
}

static bool dx12_InstallShutdownCleanupHook() {
    using namespace dx12;
    // 函数入口：RVA 主路径 + AOB 兜底 + 序言字节校验（docs/PLUGIN_GUIDE.md §5.2）
    static constexpr HookInfo kSiteShutdown = HookRva(
        0x945160, "48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 33 ED 48 8B F1 0F B7 FD");
    static const uint8_t kExpectShutdown[] = {
        0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20 };
    const uint64_t hookAddr = ResolveSite(
        kSiteShutdown, kExpectShutdown, sizeof(kExpectShutdown), "DX12/IBVBShutdown");
    if (!hookAddr) {
        return false;
    }
    // InstallHook 的 original 槽位传 nullptr：本修复不回调 trampoline。
    // （runtime 允许 origSlot 为空；即便它填了指针，我们也不使用。）
    if (!g_api->InstallHook((void*)hookAddr, (void*)&Dx12ShutdownCleanupHook, nullptr)) {
        g_api->Log("[DX12] IBVB/Shutdown hook: InstallHook failed @0x%llX",
            (unsigned long long)hookAddr);
        return false;
    }
    g_api->Log("[DX12] OK IBVB/Shutdown(RefCount) hook @0x%llX  VB.ref=%p IB.ref=%p (self-contained grow)",
        (unsigned long long)hookAddr, (void*)g_pVBRefCount, (void*)g_pIBRefCount);
    return true;
}

// DX12：全部重定向补丁 + 上限字节补丁
// 返回 true 表示全部重定向成功。
// ⚠ 只有全成功才允许继续打上限字节补丁 —— 否则会出现"阈值提高了但数组没扩容"，
//   直接越界写崩（v1 就是这么崩的：20/28 站点被静默跳过，6 个上限补丁却全生效）。
static bool dx12_ApplyRedirectPatches() {
    using namespace dx12;
    // 列表基址是运行期解析出来的，所有"原目标地址"都由它 + 固定偏移算出。
    const uint64_t ibBase = g_ibListVA;
    const uint64_t vbBase = g_vbListVA;
    const uint64_t spiBase = g_spiListVA;
    const uint64_t ibRec = (uint64_t)g_pIBRecords;
    const uint64_t ibExt = (uint64_t)g_pIBExtra;
    const uint64_t ibRef = (uint64_t)g_pIBRefCount;
    const uint64_t vbRec = (uint64_t)g_pVBRecords;
    const uint64_t vbExt = (uint64_t)g_pVBExtra;
    const uint64_t vbRef = (uint64_t)g_pVBRefCount;
    const uint64_t spiRes = (uint64_t)g_pSPIRes;
    const uint64_t spiRef = (uint64_t)g_pSPIRefCount;

    int ok = 0, fail = 0;
    auto P = [&](uintptr_t rva, uint64_t oldT, uint64_t newT, const char* n) {
        if (dx12_PatchRvaDisp(rva, oldT, newT, n)) ++ok; else ++fail;
    };

    // ---- SPI ----
    P(kSpiSiteCreateSPI, spiBase + 0x104C0, spiRes, "SPI/CreateShaderPackageInstance");
    P(kSpiSiteRenderSurface, spiBase + 0x104C0, spiRes, "SPI/RenderSurface");
    P(kSpiSiteReleaseSPI, spiBase + 0x104C0, spiRes, "SPI/ReleaseShaderPackageInstance");
    P(kSpiSiteShutdownRes, spiBase + 0x104C0, spiRes, "SPI/Shutdown(Resources)");
    P(kSpiSiteShutdownRef, spiBase + 0x80, spiRef, "SPI/Reset(refcount)");
    // AddRefShaderPackageInstance：v1[32] 就是 m_RefCount[a1]（4*a1+0x80），
    // 与 AddResource/ReleaseResource 写的是同一个数组，必须一起重定向到新数组。
    // 伪基址 = spiRef - 0x80：rcx=(spiRef-0x80)+idx*4，随后 [rcx+0x80] 正好落在新 refcount。
    // 安全性：该函数用【绝对地址】锁 cs:dword_146C1E480(= m_SPIList+0x40)，只服务 m_SPIList。
    P(kSpiSiteAddRef, spiBase, spiRef - kSpiOffRefCount, "SPI/AddRef");
    P(kSpiSiteGetEnum, spiBase + 0x104C4, spiRes + 4, "SPI/GetEnum(m_iPackageID)");
    P(kSpiSitePkgIdDisp, spiBase + 0x104C4, spiRes + 4, "SPI/CommandSubset(m_iPackageID)");

    // ---- SPI m_RefCount（+0x80）----
    // 【整条指令替换】不再手算 disp32 增量。
    // 为什么：增量法内部做 newDisp = oldDisp + delta，所以传 delta 时必须
    // 【先减掉指令里固有的偏移】（这里 = 0x80 = m_RefCount 相对 m_SPIList 的位置）。
    // 漏减会让落点整体偏 0x80，同一个句柄的引用计数被记在两个不同槽位 —— 
    // 已实测过的真实故障（AddRef 写 spiRef+i*4，Add/Release 写 spiRef+0x80+i*4）。
    // 整条替换时 disp32 就是 "目标地址 - 基址"，没有中间量可以搞错。
    //
    // 指令形态（exe 原始字节，均 mod=10 带 disp32）：
    //   0x9413A1  FF 84 8B 80 00 00 00        inc  dword [rbx+rcx*4+0x80]   (7B)
    //   0x94DAC1  42 83 84 83 80 00 00 00 FF  add  dword [rbx+r8*4+0x80],-1 (9B)
    //   0x94DACA  42 8B B4 83 80 00 00 00     mov  esi, [rbx+r8*4+0x80]    (8B)
    // 三个站点 rbx 恒为 m_SPIList（各自唯一调用者都显式传 g_SPIList）。
    {
        const int32_t disp = (int32_t)((uint32_t)(spiRef - (uint64_t)spiBase));   // g_pSPIRefCount - m_SPIList
        std::vector<uint8_t> nb;
        // ⚠ 索引寄存器【逐站点不同】，必须按原指令填：
        //   0x9413A1  inc dword [rbx+rcx*4+80h]         → rcx  (7B, 无 REX)
        //   0x94DAC1  add dword [rbx+r8 *4+80h],-1      → r8   (9B, 42=REX.B)
        //   0x94DACA  mov esi,  [rbx+r8 *4+80h]         → r8   (8B, 42=REX.B)
        // 曾经三个站点共用一个 rcx，结果后两条把 r8 编成了 rcx（长度各少 1 字节，
        // 日志里的 "8/9"、"7/8" 就是铁证）—— refcount 被减在错误的槽位上。
        struct { uintptr_t rva; int kind; asmjit::x86::Gp idx; const char* name; } sites[] = {
            { kSpiHookAdd,    0, asmjit::x86::rcx, "SPI/refcount.AddResource" },
            { kSpiHookRelAdd, 1, asmjit::x86::r8,  "SPI/refcount.Release(add)" },
            { kSpiHookRelMov, 2, asmjit::x86::r8,  "SPI/refcount.Release(mov)" },
        };
        for (auto& s : sites) {
            if (dx12_AsmMemInsn(s.kind, asmjit::x86::rbx, s.idx, disp, &nb, s.name)
                && dx12_ReplaceInsn(s.rva, nb, s.name)) ++ok;
            else ++fail;
        }
    }

    // ---- SPI：Shutdown 全表清理的两处 m_RefCount 访问 ----
    // sub_1409454D0 被 Renderer::Shutdown 调用，遍历 m_cItems 读/清 m_RefCount[idx]。
    // 形态 [rsi+rax*4+0x80]（rsi = m_SPIList），同样改用整条替换：
    //   0x945503  39 AC 86 80 00 00 00   cmp dword [rsi+rax*4+0x80], ebp
    //   0x945513  89 AC 86 80 00 00 00   mov dword [rsi+rax*4+0x80], ebp
    {
        const int32_t disp = (int32_t)((uint32_t)(spiRef - (uint64_t)spiBase));
        std::vector<uint8_t> nb;
        struct { uintptr_t rva; int kind; const char* name; } sites[] = {
            { kSpiShutCmp, 3, "SPI/Shutdown(cmp RefCount)" },
            { kSpiShutMov, 4, "SPI/Shutdown(clear RefCount)" },
        };
        for (auto& s : sites) {
            if (dx12_AsmMemInsn(s.kind, asmjit::x86::rsi, asmjit::x86::rax, disp, &nb, s.name)
                && dx12_ReplaceInsn(s.rva, nb, s.name)) ++ok;
            else ++fail;
        }
    }

    // ---- IB/VB：Shutdown 全表清理的 m_RefCount 访问（退出必崩的根因）----
    // 【这里不能打 disp32 补丁，必须用 hook，原因如下】
    //
    // sub_140945160 只有一个函数体，却被 Renderer::Shutdown 调用两次：
    //   0x950C61  rcx = &m_VBList
    //   0x950C6D  rcx = &m_IBList
    // 两条指令都是 cmp/mov [rsi+rax*4+80h]，rsi = rcx。
    // 而 "新数组 - 列表基址" 对两个列表是【不同的两个数】：
    //   g_pVBRefCount - m_VBList  !=  g_pIBRefCount - m_IBList
    // 单条指令里只有一个 disp32，无法同时表达，写死任一个都会让另一个列表读错内存。
    // 所以改为在函数入口 hook，按 rsi 分流后直接指向对应的新数组。
    //
    // 崩溃机理（逐条验证过，数字完全对得上诊断日志）：
    //   m_RefCount 起点 = 列表基址 + 0x80，容量 8192 项 -> 恰好结束于 +0x8080。
    //   而 +0x8080 正是 m_uCurPipe！紧接着 +0x8088 起是 m_DeletionPipe[0..2]。
    //   Reset(0x971490) 的 memset 长度仍是旧容量 0x8000（=8192*4），
    //   于是 idx >= 8192 时本函数读到的根本不是 refcount，而是【活跃的 pipe 结构】：
    //     idx 8192 -> m_uCurPipe          （0/1/2，非零）
    //     idx 8344 -> +0x82E0 = pipe[2] 内部（指针高位，必非零）
    //     idx 8350 -> +0x82F8 = pipe[2] 内部（必非零）
    //   这些值被当成句柄推进 m_DeleteList，Reap 再用它索引 ExtraData，
    //   拿到从未初始化过的槽位（诊断实测 slotNonZeroDwords=0/28），
    //   f50==0 -> sub_140970250 解引用 [0+0x40] -> 0xC0000005。
    //
    // 这也解释了为什么【只有退出崩】：sub_140945160 只在 Shutdown 里被调用。
    if (dx12_InstallShutdownCleanupHook()) ++ok; else ++fail;

    // ---- IB/VB ----
    P(kIBRecCreate, ibBase + 0x8400, ibRec, "IB/CreateAtHandle(records)");
    P(kIBExtCreate, ibBase + 0x58540, ibExt, "IB/CreateAtHandle(extra)");
    P(kVBRecCreate, vbBase + 0x8400, vbRec, "VB/CreateAtHandle(records)");
    P(kVBExtCreate, vbBase + 0x58540, vbExt, "VB/CreateAtHandle(extra)");
    P(kIBRecRelease, ibBase + 0x8400, ibRec, "IB/Release(records)");
    P(kIBExtRelease, ibBase + 0x58540, ibExt, "IB/Release(extra)");
    P(kVBRecRelease, vbBase + 0x8400, vbRec, "VB/Release(records)");
    P(kVBExtRelease, vbBase + 0x58540, vbExt, "VB/Release(extra)");
    P(kIBRecReap, ibBase + 0x8400, ibRec, "IB/Reap(records)");
    P(kIBExtReap, ibBase + 0x58540, ibExt, "IB/Reap(extra)");
    P(kVBRecReap, vbBase + 0x8400, vbRec, "VB/Reap(records)");
    P(kVBExtReap, vbBase + 0x58540, vbExt, "VB/Reap(extra)");
    P(kIBRecReset, ibBase + 0x8400, ibRec, "IB/Reset(records)");
    P(kVBRecReset, vbBase + 0x8400, vbRec, "VB/Reset(records)");
    P(kIBRefReset, ibBase + 0x80, ibRef, "IB/Reset(refcount)");
    P(kVBRefReset, vbBase + 0x80, vbRef, "VB/Reset(refcount)");
    // 同一条 Reset 里紧跟 lea 的 memset 长度必须一起改，否则只清旧容量，
    // 索引 >= 旧容量的引用计数会残留到下一局（详见 dx12_PatchMovImm32 的注释）。
    if (dx12_PatchMovImm32(kIBRefResetLen, 0x8000, 0x10000, "IB/Reset(memset 16384)")) ++ok; else ++fail;
    if (dx12_PatchMovImm32(kVBRefResetLen, 0x8000, 0x10000, "VB/Reset(memset 16384)")) ++ok; else ++fail;
    if (dx12_PatchMovImm32(kSPIRefResetLen, 0x10000, 0x20000, "SPI/Reset(memset 32768)")) ++ok; else ++fail;
    P(kVBRecRelease2, vbBase + 0x8400, vbRec, "VB/Release2(records)");
    P(kVBExtRelease2, vbBase + 0x58540, vbExt, "VB/Release2(extra)");
    P(kIBRecIndex, ibBase + 0x8400, ibRec, "IB/SetVbIb(records)");
    P(kVBIxIndex, vbBase + 0x8400, vbRec, "VB/SetVbIb(records)");

    // ---- IB/VB："列表基址 + 0x8400" 形态（不在 xrefs_to 的结果里，必须单独处理）----
    // 目标：让 &m_IBList[5*idx] + imm 指向插件的新记录数组
    {
        const int64_t ibDelta = (int64_t)ibRec - (int64_t)g_ibListVA;
        const int64_t vbDelta = (int64_t)vbRec - (int64_t)g_vbListVA;
        if (dx12_PatchAddImm32(kIBRecBaseAdd, ibDelta, "IB/Release(base+0x8400)")) ++ok; else ++fail;
        if (dx12_PatchAddImm32(kVBRecBaseAdd, vbDelta, "VB/Release(base+0x8400)")) ++ok; else ++fail;
    }

    // ---- AddRefIB/AddRefVB：整个 RefCount 数组的重定向（基址形式，之前漏了）----
    // DX11 对应 0x9404F2 / 0x9405E2，把 `lea rcx, m_List` 换成 RefCount-0x80 的伪基址，
    // 使随后的 [rcx+0x80+idx*4] 正好落在 RefCount[idx]。
    if (dx12_PatchRvaDisp(kIBAddRefBase, g_ibListVA, ibRef - 0x80, "IB/AddRef(refcount)")) ++ok; else ++fail;
    if (dx12_PatchRvaDisp(kVBAddRefBase, g_vbListVA, vbRef - 0x80, "VB/AddRef(refcount)")) ++ok; else ++fail;

    // ---- ReleaseIB/ReleaseVB 的 ExtraData 访问（基址+偏移形式，之前漏了）----
    //   lea rdx, [rcx+58540h]      -> ExtraData 数组基址
    //   mov edi, [rax+rcx+585A8h]  -> ExtraData[idx].f68
    // disp32 是【相对列表基址】的偏移：
    //   旧 disp = 0x58540（lea 的 ExtraData 基址）/ 0x585A8（mov 的 +0x68）
    //   新 disp 必须 = (新 ExtraData - 列表基址) [+ 0x68]，两者相差同一个常量
    //   → delta = 新ExtraData - 列表基址 - 0x58540
    // 【2026-10-17 修正】原先写成 +0x58540，等价于多跳一个 0x58540（约 3230 项），
    //   ExtraData[idx] 读到 idx+3230 那一项；对局中 ReadbackVB 取到未初始化项，
    //   +0x50 为 0 → sub_14096D510 里 lock cmpxchg [0] 崩溃（handle 0x1EC2）。
    {
        const int64_t ibExtDelta = (int64_t)ibExt - (int64_t)ibBase - 0x58540;
        const int64_t vbExtDelta = (int64_t)vbExt - (int64_t)vbBase - 0x58540;
        if (dx12_PatchMemDisp(kIBRelExtBase, ibExtDelta, "IB/Release(extra base)")) ++ok; else ++fail;
        if (dx12_PatchMemDisp(kIBRelExt68,   ibExtDelta, "IB/Release(extra+68)"))  ++ok; else ++fail;
        if (dx12_PatchMemDisp(kVBRelExtBase, vbExtDelta, "VB/Release(extra base)")) ++ok; else ++fail;
        if (dx12_PatchMemDisp(kVBRelExt68,   vbExtDelta, "VB/Release(extra+68)"))  ++ok; else ++fail;
    }

    // ---- SetVbIb 的记录字段读（绝对地址形式，之前漏了）----
    // 原目标 = 记录数组基址 + 字段偏移；新目标 = 新记录数组 + 同偏移
    if (dx12_PatchRvaDisp(kIBRecF1C, ibBase + 0x841C, ibRec + 0x1C, "IB/SetVbIb rec.f1C")) ++ok; else ++fail;
    if (dx12_PatchRvaDisp(kIBRecF18, ibBase + 0x8418, ibRec + 0x18, "IB/SetVbIb rec.f18")) ++ok; else ++fail;
    if (dx12_PatchRvaDisp(kVBRecF1C, vbBase + 0x841C, vbRec + 0x1C, "VB/SetVbIb rec.f1C")) ++ok; else ++fail;
    if (dx12_PatchRvaDisp(kVBRecF18, vbBase + 0x8418, vbRec + 0x18, "VB/SetVbIb rec.f18")) ++ok; else ++fail;
    if (dx12_PatchRvaDisp(kVBRecF20, vbBase + 0x8420, vbRec + 0x20, "VB/SetVbIb rec.f20")) ++ok; else ++fail;

    // ---- records 数组其余字段的读取（f10/f28/f40/f44/f110）----
    // 原目标 = 旧 records 基址 + 字段偏移；新目标 = 新 records 基址 + 同偏移。
    // 这些是【读路径】；写路径由 CreateIBAtHandle 派生的指针负责，无需单独处理。
    if (dx12_PatchRvaDisp(kIBRecF10,  ibBase + 0x8410, ibRec + 0x10,  "IB/rec.f10 (崩溃点)")) ++ok; else ++fail;
    if (dx12_PatchRvaDisp(kVBRecF10,  vbBase + 0x8410, vbRec + 0x10,  "VB/rec.f10")) ++ok; else ++fail;
    if (dx12_PatchRvaDisp(kIBRecF28,  ibBase + 0x8428, ibRec + 0x28,  "IB/rec.f28")) ++ok; else ++fail;
    if (dx12_PatchRvaDisp(kVBRecF28,  vbBase + 0x8428, vbRec + 0x28,  "VB/rec.f28")) ++ok; else ++fail;
    if (dx12_PatchRvaDisp(kIBRecF40,  ibBase + 0x8440, ibRec + 0x40,  "IB/rec.f40")) ++ok; else ++fail;
    if (dx12_PatchRvaDisp(kIBRecF44,  ibBase + 0x8444, ibRec + 0x44,  "IB/rec.f44")) ++ok; else ++fail;
    if (dx12_PatchRvaDisp(kVBRecF40,  vbBase + 0x8440, vbRec + 0x40,  "VB/rec.f40")) ++ok; else ++fail;
    if (dx12_PatchRvaDisp(kVBRecF44,  vbBase + 0x8444, vbRec + 0x44,  "VB/rec.f44")) ++ok; else ++fail;
    // 注意：kIBRecF110 / kVBRecF110 两处【不能】当成记录字段访问来改。
    //   0xF99BD3 = lea rbx,[m_IBList+0x8510]，0x8510 = m_DeletionPipe[3] 的起跑线；
    //   析构循环每次 -=0x128，三次退到 pipe[2..0]。改起点会让减法落进 records，
    //   把记录数据当堆指针 free。

    g_api->Log("[DX12] redirects: %d ok, %d failed", ok, fail);
    return fail == 0;
}


// DX12 入口。顺序与安全门：
//   1. 分配扩展数组（必须落在 imagebase +- 2GB 内）
//   2. 安装 4 个 hook
//   3. 应用 20 个重定向 —— 必须全部成功
//   4. 才允许打 6 个上限字节补丁
//
// 第 3->4 之间的门是必需的：上限补丁把 m_cItems 阈值从 8192/16384 提到
// 16384/32768，若数组没同步扩容就是必然的越界写。宁可不加容量，也不能崩。
void UnlockModelLimit_Dx12_Init() {
    using namespace dx12;
    g_api->Log("[DX12] === DX12 branch (all redirects + hooks + limit patches) ===");
    if (!dx12_ResolveListBases()) {
        return;
    }
    if (!dx12_InitExtendedArrays()) {
        return;
    }
    dx12_InstallHooks();

    if (!dx12_ApplyRedirectPatches()) {
        g_api->Log("[DX12] redirects incomplete - skipping limit patches (fail-safe)");
        return;
    }

    dx12_PatchLimitByte(kPatchIBCmp,     0x40, "IB/VB cmp 2000h->4000h");
    dx12_PatchLimitByte(kPatchIBMovEdx,  0x40, "IB/VB mov edx 2000h->4000h");
    dx12_PatchLimitByte(kPatchIBMovR8d,  0x40, "IB/VB mov r8d 2000h->4000h");
    dx12_PatchLimitByte(kPatchSPICmp,    0x80, "SPI cmp 4000h->8000h");
    dx12_PatchLimitByte(kPatchSPIMovEdx, 0x80, "SPI mov edx 4000h->8000h");
    dx12_PatchLimitByte(kPatchSPIMovR8d, 0x80, "SPI mov r8d 4000h->8000h");

    g_api->Log("UnlockModelLimit[DX12]: installed (%zu patches)", g_patchedAddrs.size());
}
