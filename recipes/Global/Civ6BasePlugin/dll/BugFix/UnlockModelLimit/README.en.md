# UnlockModelLimit

> English | [中文](README.md)

> Category: **BugFix** · Back to [project overview](../../../README.md)

Lift the engine's model / material count limit so that large mods do not crash.

## Problem

The engine tracks render resource objects in **static arrays**, and the array lengths are hard-coded:

| Resource object | Original limit | After extension |
| --- | --- | --- |
| IB / VB (Index / Vertex Buffer) | 8192 | 16384 |
| SPI (ShaderPackageInstance) | 16384 | 32768 |

Once vanilla assets pile up it errors out or even fails to render. As soon as the mod scale grows (many custom models / materials) it is bound to be hit.

## Approach

**The engine's static arrays themselves are not modified**; instead, handles beyond the original limit are **rewritten into extended arrays allocated by the plugin**.

```
handle < original limit  → use the engine's original array (untouched)
handle ≥ original limit  → rewrite the index, landing in the plugin's extended array
```

### Four implementation steps

1. **Extended arrays**: `VirtualAlloc` allocates them **within imagebase ±2GB** (guaranteeing disp32 reachability).
   The structure of each table is `RefCount / records / ExtraData`, reserved for the full u16 handle space (65536 entries).

2. **Redirection**: retarget the engine instructions that read and write these arrays to the extended arrays. Three forms are handled separately:

   | Instruction form | Handling |
   | --- | --- |
   | `lea rcx, [rip+disp32]` | change the disp32 only |
   | `[base+idx*4+80h]` indexed access | **re-encode the whole instruction** with asmjit |
   | Number of hook sites | varies with the exe (DX12 currently 58) |

3. **hooks** (the count differs per exe branch):

   | Branch | Count | Details |
   | --- | --- | --- |
   | DX11 | **4** | `AddResourceSPI` / `ReleaseSPI` / `AddResource` / `Release` (log `%d/4 hooks`) |
   | DX12 | **3** | the refcount instructions of `AddResource` / `Release` + whole-function replacement of `IBVB::Shutdown` |

   What both branches share: `AddResource` / `Release` are used by both IB and VB, and the target array is decided at runtime by `this`,
   **which a single disp32 cannot express**; DX12 additionally replaces the whole `IBVB::Shutdown` function (dispatching by `rsi` to the corresponding extended array,
   and implementing the `m_DeleteList` growth itself).

4. **Limit constants**: the 6 immediate bytes are changed **last**.
   ⚠️ "the preceding redirections must all succeed, otherwise the whole group is skipped" **holds only in the DX12 branch**:
   DX11's three `Apply*RedirectPatches` / `ApplyIBVB*Patches` return `void`, a single hook site failure only writes one log line,
   and `ApplyLimitBytePatches()` still runs unconditionally. DX11's failure is **per-hook-site degradation**, not giving up on the whole group.

### fail-safe design

The order must not be scrambled: redirect first → then install hooks → finally change the constants.

- **DX12**: failure at any step means **abandoning the whole thing** — no hooks installed, no patches applied, no limit constants changed, the game stays as it was (log `redirects incomplete`).
- **DX11**: three steps, each doing its best per hook site internally (a failure only logs), and the three steps do not veto one another.
  So when some hook sites fail in DX11, the feature is **partially in effect** rather than skipped as a whole.

The hard constraint shared by both branches is the **order**: the reverse (changing the constants before redirecting) makes handles exceed the initialization range of the extended arrays and crashes outright.

## Compatibility design

- **Global objects do not hard-code addresses**: `m_IBList` / `m_VBList` / `m_SPIList` / `m_ComputeSPIList`
  use `ResolveDataByAob` to **resolve the disp32 on the fly** from the `lea` instructions that reference them;
- **Hook sites go through `HookInfo`** (RVA main path + AOB fallback): when the bytes at the RVA landing point do not match, it automatically switches to the AOB,
  and if neither matches, installation is abandoned;
- **Every byte patch validates** the original instruction form and the expected immediate; on a mismatch it refuses to write.

## Dual-exe adaptation

The DX11 and DX12 exes are two binaries, and their hook site RVAs and forms differ:

| Branch | File |
| --- | --- |
| DX11 | `UnlockModelLimit.cpp` |
| DX12 | `UnlockModelLimit_Dx12.cpp` |
| Shared utilities | `UnlockModelLimit_Common.h` (Patch / decoding / hook installation / hook site resolution) |

The current exe is distinguished automatically at runtime.

## Two asmjit pitfalls (measured)

Encountered while writing trampolines / re-encoding instructions:

| Form | What is actually generated | Explanation |
| --- | --- | --- |
| `a.jmp((uint64_t)addr)` | `FF 25 02 00 00 00` + inline pointer (16 bytes) | **Indirect jump**: `FF 25 rel32` is `jmp qword ptr [rip+rel32]`, and `rel32=2` here points exactly at the inline pointer slot right after it. So it **does** jump to `addr`, but that 16-byte form is incompatible with the "6-byte `E9 rel32`" — anything that computes backfill length and overwrite width as 6 bytes ends up misaligned. **This module no longer uses it**; whole-instruction re-encoding is now used throughout |
| `a.jmp(x86::ptr(addr))` | `RelocOffsetOutOfRange` | errors out directly, unusable |
| `a.jmp(Label)` | `E9 xx xx xx xx` | can generate `E9 rel32`, but a `Label` can only point inside the same `CodeHolder` |
| `a.embedUInt8(0xE9); a.embedInt32(rel)` | `E9 ...` | ✅ workable: asmjit handles the encoding, and the target address is backfilled later |

> The first three rows of the table are **pitfall records** (the current code no longer contains any use of `jmp((uint64_t))` / `FF 25` / `embedUInt8(0xE9)`).
> What the DX12 branch does now is re-encode each rewritten instruction **as a whole** with asmjit, without hand-writing `E9`.

Also, **asmjit only writes an illegal operand into the emitter's local `err`**, `rt.add()` still returns `kErrorOk`,
and that instruction is silently dropped — so every emit must be checked by the caller (see `ASMJIT_EMIT` in
`UnlockModelLimit_Common.h`).

## Origin

The patch logic originates from **DllProxy** (the `version.dll` proxy approach), later reworked into the HookAPI plugin form.

## Logging

The log format differs per branch (below is the format string verbatim; `%zu` / `%d` are runtime values, do not copy them as fixed numbers):

```
UnlockModelLimit: exe image = DX12
[DX12] hooks installed: %d/2
UnlockModelLimit[DX12]: installed (%zu patches)
```
```
UnlockModelLimit: exe image = DX11
UnlockModelLimit: installed (%zu patches, %d/4 hooks)
```

- If DX12 prints `redirects incomplete`, the hook site layout of that exe version has changed and the patches are **skipped as a whole**
  (the feature does not take effect, but memory is not modified).
- DX11 has no such whole-group gate: hook site failures are logged one by one, and in the end only part may take effect (see the fail-safe section above).
- Neither branch prints a fixed hook site count — the set of hook sites varies with the exe version, and the RVA list in the `.cpp` is the source of truth.
