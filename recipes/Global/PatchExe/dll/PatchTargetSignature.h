#pragma once

#include <cstddef>
#include <cstdint>

// Verified against DX11 CivilizationVI.exe 1.0.12.68 (1023995), timestamp
// 0x667C6FC6, SizeOfImage 0x036D1000. SHA-256:
// 8396ACA228300A47A79A18697A927C8A4526B317E689558DDC24925F1523B99D
// A local instruction signature, not a guarantee of compatibility with every
// executable version. ResolveByRva maps these anchor coordinates first.
// Keep the window away from AddResource's entry (0x940770), so HookExe's
// entry detour can coexist with this patch.
static constexpr uintptr_t kPatchWindowRva = 0x9407C9;
static constexpr size_t kPatchByteOffset = 6;
static constexpr uint8_t kPatchExpectedBytes[] = {
    0x8B, 0x03,                         // mov eax, [rbx]
    0xFF, 0xC0,                         // inc eax
    0x3D, 0x00, 0x20, 0x00, 0x00,       // cmp eax, 0x2000
    0x73, 0x0A,                         // jae capacity-error path
    0x0F, 0xB7, 0x3B,                   // movzx edi, word ptr [rbx]
    0x89, 0x03,                         // mov [rbx], eax
};
static_assert(kPatchWindowRva + kPatchByteOffset == 0x9407CF);
static_assert(kPatchExpectedBytes[kPatchByteOffset] == 0x20);
