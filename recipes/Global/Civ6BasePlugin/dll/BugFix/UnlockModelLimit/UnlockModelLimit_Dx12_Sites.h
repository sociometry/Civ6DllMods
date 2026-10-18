#pragma once
#include <cstdint>

// 自动生成：DX12 补丁站点的指令级 AOB（RVA 解析失败时兜底）。
//
// 生成依据：当前 CivilizationVI_DX12.exe（TimeDateStamp 0x667C7363）。
// 每条 AOB 都在 .text 里验证过【唯一命中】；指令内的 disp32/imm32 用 ?? 通配，
// 因此数组地址变化时仍能定位（指令结构变化则失效，此时按 fail-closed 跳过）。
//
// rva   = 调用侧传入的 RVA（PatchLimitByte 是立即数字节的位置）
// delta = rva - 指令起点；AOB 命中的是指令起点，加 delta 还原到操作位置。
struct Dx12SiteAob {
    uintptr_t   rva;
    const char* aob;
    int         delta;
    const char* name;
};

constexpr Dx12SiteAob kDx12SiteAobs[] = {
    { 0x944A77, "FF FF 44 0F B7 C8 48 8D 05 ?? ?? ?? ?? 45 8B C1 49 C1 E0", 0, "kSpiSiteCreateSPI" },
    { 0x95E20A, "41 57 48 83 EC 38 48 8D 05 ?? ?? ?? ?? 45 8B E8 49 C1 E5", 0, "kSpiSiteRenderSurface" },
    { 0x94DD35, "3C 48 89 7C 24 38 48 8D 05 ?? ?? ?? ?? 0F B7 FB 33 DB 48", 0, "kSpiSiteReleaseSPI" },
    { 0x97175B, "89 35 85 55 8B 0E 48 8D 0D ?? ?? ?? ?? 89 35 40 55 0A 07", 0, "kSpiSiteShutdownRes" },
    { 0x97177F, "93 EC 01 00 33 D2 48 8D 0D ?? ?? ?? ?? 41 B8 00 00 01 00", 0, "kSpiSiteShutdownRef" },
    { 0x940802, "DC 2D 06 75 F2 0F B7 C1 48 8D 0D ?? ?? ?? ?? 48 8D 0C 81 8B 81 80 00", 0, "kSpiSiteAddRef" },
    { 0x9479E3, "C3 CC CC 0F B7 C1 48 8D 0D ?? ?? ?? ?? 48 03 C0 8B 04 C1", 0, "kSpiSiteGetEnum" },
    { 0x95C2E3, "0F B7 C2 48 03 C0 41 8B 84 C5 ?? ?? ?? ?? 48 69 C8 20 04 00", 0, "kSpiSitePkgIdDisp" },
    { 0x96A4FA, "F9 B8 02 00 00 00 48 8D 0D ?? ?? ?? ?? 41 8B D8 41 BA 04", 0, "kIBRecCreate" },
    { 0x96A51E, "04 BF 4C 8D 1C C1 48 8D 05 ?? ?? ?? ?? 4C 03 C0 49 8B 70", 0, "kIBExtCreate" },
    { 0x96CD79, "0F AF FB 44 8B C0 48 8D 05 ?? ?? ?? ?? 4E 8D 1C C8 4C 6B", 0, "kVBRecCreate" },
    { 0x96CD88, "1C C8 4C 6B CD 70 48 8D 05 ?? ?? ?? ?? 4C 03 C8 49 8B 59", 0, "kVBExtCreate" },
    { 0x94A565, "8D 04 80 48 03 D1 48 8D 0D ?? ?? ?? ?? 48 8D 0C C1 E9 9B", 0, "kIBRecRelease" },
    { 0x94A553, "02 00 CC CC CC 0F B7 C1 48 8D 0D ?? ?? ?? ?? 48 6B D0 70 48 8D 04 80", 0, "kIBExtRelease" },
    { 0x94A6C5, "8D 04 80 48 03 D1 48 8D 0D ?? ?? ?? ?? 48 8D 0C C1 E9 3B", 0, "kVBRecRelease" },
    { 0x94A6B3, "5C C3 CC CC CC 0F B7 C1 48 8D 0D ?? ?? ?? ?? 48 6B D0 70 48 8D 04 80", 0, "kVBExtRelease" },
    { 0x96E1ED, "00 48 8D 04 9B BA 28 00 00 00 49 8D 9C 24 ?? ?? ?? ?? 48 8D 1C C3 48 8B CB E8 EF 1D", 0, "kIBRecReap" },
    { 0x96E1C7, "8B 05 89 C3 35 06 49 8D BC 24 ?? ?? ?? ?? 8B CE 0F B7 1C 48", 0, "kIBExtReap" },
    { 0x96E16D, "00 48 8D 04 9B BA 28 00 00 00 49 8D 9C 24 ?? ?? ?? ?? 48 8D 1C C3 48 8B CB E8 6F 1E", 0, "kVBRecReap" },
    { 0x96E147, "8B 05 89 F4 99 0E 49 8D BC 24 ?? ?? ?? ?? 8B CE 0F B7 1C 48", 0, "kVBExtReap" },
    { 0x971658, "89 35 68 8A 30 06 48 8D 0D ?? ?? ?? ?? E8 AC ED 01 00 33", 0, "kIBRecReset" },
    { 0x9715F2, "89 35 4E BB 94 0E 48 8D 0D ?? ?? ?? ?? E8 12 EE 01 00 33", 0, "kVBRecReset" },
    { 0x971666, "AC ED 01 00 33 D2 48 8D 0D ?? ?? ?? ?? 41 B8 00 80 00 00", 0, "kIBRefReset" },
    { 0x971600, "12 EE 01 00 33 D2 48 8D 0D ?? ?? ?? ?? 41 B8 00 80 00 00", 0, "kVBRefReset" },
    { 0x96C297, "1C 76 48 C1 E3 06 4D 8D A0 ?? ?? ?? ?? 48 03 F3 4D 8D 24", 0, "kVBRecRelease2" },
    { 0x96C2AC, "8D A8 58 4A E5 06 48 8D 05 ?? ?? ?? ?? 4C 03 EB 4C 03 C8", 0, "kVBExtRelease2" },
    { 0x95F62A, "D0 48 8D 04 92 48 03 C8 48 8B 84 CE ?? ?? ?? ?? 48 89 44 24 20 48 8D 04", 0, "kIBRecIndex" },
    { 0x95F6D7, "D3 48 8D 04 92 48 03 C8 48 8B 84 CE ?? ?? ?? ?? 48 89 44 24 20 48 8D 04", 0, "kVBIxIndex" },
    { 0x94A4A5, "04 80 48 8D 0C C1 48 81 C1 00 84 00 00 E8 5F", 0, "kIBRecBaseAdd" },
    { 0x94A505, "04 80 48 8D 0C C1 48 81 C1 00 84 00 00 E8 FF", 0, "kVBRecBaseAdd" },
    { 0x9407B2, "18 33 06 75 F2 0F B7 C1 48 8D 0D ?? ?? ?? ?? 48 8D 0C 81 8B 81 80 00", 0, "kIBAddRefBase" },
    { 0x9408A2, "0E 75 F2 0F B7 C1 48 8D 0D ?? ?? ?? ?? 48 8D 0C 81 8B 81", 0, "kVBAddRefBase" },
    { 0x94A48C, "8D 0D B4 7B 32 06 48 8D 91 ?? ?? ?? ?? 48 03 D0 8B BC 08", 0, "kIBRelExtBase" },
    { 0x94A496, "32 06 48 8D 91 40 85 05 00 48 03 D0 8B BC 08 ?? ?? ?? ?? 4B 8D 04 80 48 8D 0C C1 48 81 C1 00", 0, "kIBRelExt68" },
    { 0x94A4EC, "8D 0D D4 AB 96 0E 48 8D 91 ?? ?? ?? ?? 48 03 D0 8B BC 08", 0, "kVBRelExtBase" },
    { 0x94A4F6, "96 0E 48 8D 91 40 85 05 00 48 03 D0 8B BC 08 ?? ?? ?? ?? 4B 8D 04 80 48 8D 0C C1 48 81 C1 00", 0, "kVBRelExt68" },
    { 0x95F643, "0C C5 00 00 00 00 8B 84 31 ?? ?? ?? ?? 48 8D 54 24 20 89", 0, "kIBRecF1C" },
    { 0x95F653, "24 20 89 44 24 28 8B 84 31 ?? ?? ?? ?? 89 44 24 2C 48 8B", 0, "kIBRecF18" },
    { 0x95F6F2, "00 00 00 00 33 D2 8B 84 33 ?? ?? ?? ?? 89 44 24 28 8B 84", 0, "kVBRecF1C" },
    { 0x95F6FD, "2B 0F 89 44 24 28 8B 84 33 ?? ?? ?? ?? 89 44 24 2C 44 8D", 0, "kVBRecF18" },
    { 0x95F71F, "FF 90 60 01 00 00 8B 84 33 ?? ?? ?? ?? 39 87 E8 01 00 00", 0, "kVBRecF20" },
    { 0x95FF14, "8B 0F 48 8D 14 80 48 8B 94 D3 ?? ?? ?? ?? E8 AF 50 00 00 41", 0, "kIBRecF10" },
    { 0x95FEF7, "8B 0F 48 8D 14 80 48 8B 94 D3 ?? ?? ?? ?? E8 CC 50 00 00 41", 0, "kVBRecF10" },
    { 0x95D51B, "00 00 48 8D 55 90 48 8B 84 C3 ?? ?? ?? ?? 48 89 45 90 8B 05", 0, "kIBRecF28" },
    { 0x95D4E1, "D2 44 8D 47 01 48 8B 84 C3 ?? ?? ?? ?? 48 89 45 80 8B 05", 0, "kVBRecF28" },
    { 0x95D530, "CF 31 06 89 45 98 8B 05 ?? ?? ?? ?? 89 45 9C 48 8B 06", 0, "kIBRecF40" },
    { 0x95D527, "C7 06 48 89 45 90 8B 05 ?? ?? ?? ?? 89 45 98 8B 05 4A", 0, "kIBRecF44" },
    { 0x95D4F5, "00 96 0E 89 45 88 8B 05 ?? ?? ?? ?? 89 45 8C 48 8B 06", 0, "kVBRecF40" },
    { 0x95D4EC, "2B 0F 48 89 45 80 8B 05 ?? ?? ?? ?? 89 45 88 8B 05 05", 0, "kVBRecF44" },
    { 0x9413A1, "D9 42 95 00 4C 8D 9C 24 80 00 00 00 0F B7 CF 0F B7 C7 FF 84 8B ?? ?? ?? ?? 89 73 40 49 8B 5B 10 49 8B 73 18 49 8B E3 5F C3 CC CC", 0, "kSpiHookAdd" },
    { 0x94DAC1, "90 33 C0 F0 0F B1 4B 40 75 F5 44 0F B7 44 24 38 42 83 84 83 ?? ?? ?? ?? FF 42 8B B4 83 80 00 00 00 75 5F 8B 83 80 00 01 00", 0, "kSpiHookRelAdd" },
    { 0x94DACA, "83 84 83 80 00 00 00 FF 42 8B B4 83 ?? ?? ?? ?? 75 5F 8B 83 80 00 01 00", 0, "kSpiHookRelMov" },
    { 0x945503, "00 00 00 00 00 0F B7 C7 39 AC 86 ?? ?? ?? ?? 74 4E 48 8D 9E C0 04 05", 0, "kSpiShutCmp" },
    { 0x945513, "8D 9E C0 04 05 00 89 AC 86 ?? ?? ?? ?? 8B 83 20 04 00 00", 0, "kSpiShutMov" },
    { 0x940C3F, "85 05 00 E9 CA 00 00 00 8B 03 FF C0 3D ?? ?? ?? ?? 73 0A 0F B7 3B 89 03 E9 B5 00 00 00", 2, "kPatchIBCmp" },
    { 0x940C6E, "E8 64 77 05 00 BA 00 20 00 00 48 8D 0D E8", 1, "kPatchIBMovEdx" },
    { 0x940CD4, "00 E8 5F 6C 05 00 41 B8 ?? ?? ?? ?? 48 8D 15 D2 DD 72", 3, "kPatchIBMovR8d" },
    { 0x9412CF, "08 05 00 E9 CA 00 00 00 8B 03 FF C0 3D ?? ?? ?? ?? 73 0A 0F B7 3B 89 03 E9 B5 00 00 00", 2, "kPatchSPICmp" },
    { 0x9412FE, "E8 D4 70 05 00 BA 00 40 00 00 48 8D 0D 58 D7", 1, "kPatchSPIMovEdx" },
    { 0x941364, "00 E8 CF 65 05 00 41 B8 ?? ?? ?? ?? 48 8D 15 42 D7 72", 3, "kPatchSPIMovR8d" },
};
