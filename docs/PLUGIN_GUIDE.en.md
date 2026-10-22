# Civ6DllLoader Plugin Development Guide

> English | [中文](PLUGIN_GUIDE.md)

Audience: **mod authors developing Civilization VI C/C++ feature plugins**. There is no need to read or modify the Loader source; just include `HookAPI.h` in your project, declare the plugin category as specified and implement `OnLoad`, then put the compiled DLL into the corresponding Mod plugin subdirectory.

---

## 1. Core Architecture and Runtime Mechanism

Civilization VI natively allows only a single GameCore DLL to be specified in `Config.sql`, so multiple mods that modify GameCore are mutually exclusive. Civ6DllLoader uses a dual-track architecture to let multiple mods coexist:

```text
[Steady-state run (version.dll installed)]
Game startup
  → Load version.dll from the game root directory (static import hijack)
  → Start Civ6DllLoader.runtime.dll (Phase A: resident exe domain)
  → Hook GameCore::LoadDLL
  → When the engine requests loading GameCore, the detour automatically redirects back to the official original GameCore DLL (the shell is never loaded)
  → Take over the GameCore lifecycle (Phase B), automatically scan and load each Mod's plugin DLLs
  → After the main menu is built, mount each Mod directory's FrontEndUI_*.xml into the main menu (no plugin DLL required)

[First-time bootstrap (version.dll not installed)]
Player installs a Mod carrying the shell and enters the game
  → The engine slot loads the shell GameCore_XP2_Shell_FinalRelease.dll
  → The shell scans enabled mods, runs the election and starts the highest-version runtime.dll
  → Automatically installs its own takeover as version.dll in the game root (foreign dlls are backed up and taken over)
  → Prompts the player to restart the game; afterwards it enters steady-state run
```

### Deliverable File Responsibilities

| File | Source | Placement and responsibility in the Mod |
|------|------|-------------------|
| `GameCore_XP2_Shell_FinalRelease.dll` | Loader release package (shell) | Place as-is into the Mod's `Binaries/Win64/`, used for first-time bootstrap and automatic takeover when `version.dll` is not installed |
| `Civ6DllLoader.runtime.dll` | Loader release package (runtime core) | Place as-is into the Mod's `Binaries/Win64/`; when multiple Mods coexist, the highest version wins the election and is started automatically |
| `Plugins/Global/*.dll` or `Plugins/GameCore/*.dll` | **Compiled by yourself** | Your plugin DLL, exporting `OnLoad` (optionally exporting `OnMessage`; no need to implement `OnUnload`) |
| `HookAPI.h` | Loader release package | The only C/C++ interface header your plugin project needs to include |
| `UI/FrontEnd/FrontEndUI_*.xml` + `.lua` | **Written by yourself** | Main menu interface, **no compilation needed**; just put it into the Mod directory, no registration required (see §7) |

---

## 2. Mod Directory Structure Specification

### 2.0 Remember These Four Points First

Before writing a plugin, confirm these four things; they are the most common causes of failure:

1. **Plugin DLLs must be placed in a subdirectory**: `Plugins/Global/` or `Plugins/GameCore/`. DLLs placed directly in the `Plugins/` root directory are ignored by the scanner and will not be loaded.
2. **The project must define a category macro**: either `CIV6_PLUGIN_GLOBAL` or `CIV6_PLUGIN_GAMECORE`. If neither is defined, `HookAPI.h` triggers `#error` and refuses to compile.
3. **The project must carry a version resource** (`VERSIONINFO` in a `.rc`). **Plugins without a file version will not be loaded**—see §2.2.
4. **Hooks and patches are mutually exclusive**: the same address cannot be both `InstallHook` and `Patch`; both directions are rejected. To change an already-hooked function, do it inside the detour (see §6.3 for details).

### 2.1 Directory Template

The physical directory of each Mod must be organized as follows:

```text
MyAwesomeMod/
  MyAwesomeMod.modinfo          ← UTF-8 without BOM
  Data/
    Config.sql                  ← used for first-time bootstrap takeover; fill PackageId with this modinfo's id
  UI/FrontEnd/                  ← main menu interface (as needed, see §7; no compilation needed)
    FrontEndUI_MyMod.xml
    FrontEndUI_MyMod.lua
  Binaries/Win64/
    GameCore_XP2_Shell_FinalRelease.dll
    Civ6DllLoader.runtime.dll
    Plugins/
      Global/                   ← global resident plugin directory (choose as needed)
        MyGlobalPlugin.dll
      GameCore/                 ← match lifecycle plugin directory (choose as needed)
        MyGameplayPlugin.dll
```

> **Important scanning rule**: the Loader scans plugins strictly by subdirectory:
> - `Binaries/Win64/Plugins/Global/*.dll`: recognized as **Global plugins**.
> - `Binaries/Win64/Plugins/GameCore/*.dll`: recognized as **GameCore plugins**.
> - **Never place plugin DLLs directly in the `Plugins/` root directory**; DLLs placed directly in the root directory are ignored by the scanner and cannot be loaded.

> Mods that only provide a main menu interface and need no C++ logic can **skip DLL compilation entirely**—keep only the shell + runtime plus
> the pair of `FrontEndUI_*.xml` / `.lua` under `UI/FrontEnd/` (see §7).

### 2.2 Version Resource (Mandatory)

**Plugin DLLs must carry a `VERSIONINFO` file version, otherwise the Loader will not load them.**

This is not a suggestion but a pre-load gate: DLLs whose version cannot be read during the scan are rejected outright, leaving one line in the log:
`Rejected plugin (no VERSIONINFO file version resource): <path>`.

#### How to add it

Add a `.rc` file to the plugin project (you can copy the content below directly), then add it to the `.vcxproj`:

```xml
<ItemGroup>
  <ResourceCompile Include="Version.rc" />
</ItemGroup>
```

```rc
VS_VERSION_INFO VERSIONINFO
 FILEVERSION 1,0,0,0
BEGIN
END
```

That is all three lines. **The whole file contains only one version number; to change the version, change only the `FILEVERSION` line.**

#### How the Loader uses it

**① No version → not loaded** (the gate described above).

**② Only one copy of a same-named plugin is loaded, keeping the highest version.**

When several Mods each ship a copy of the same kind of plugin (for example, all derived from the same base plugin), the Loader groups them by
**file name (case-insensitive)** and keeps only the copy with the **highest file version** in each group:

```
ModA/Plugins/GameCore/Foo.dll   v1.0.0.0   ← dropped
ModB/Plugins/GameCore/Foo.dll   v1.2.0.0   ← loaded
ModC/Plugins/GameCore/Foo.dll   v1.1.0.0   ← dropped
```

The arbitration result can be seen in the log:

```
Duplicate plugin basename: kept <ModB's path> (v1.2.0.0), dropped <ModA's path> (v1.0.0.0, lower version)
```

**When versions are equal**, the decision is made by lexicographic path order (case-insensitive), guaranteeing the same result on every startup on the same machine:

```
Duplicate plugin basename: kept ... (v1.0.0.0), dropped ... (v1.0.0.0, same version -> lexicographic tie-break)
```

#### A few things to note

- **Only plugins in enabled Mods take part in the competition**. A disabled Mod does not even count as a candidate;
- **`Global` and `GameCore` are grouped independently**. `Plugins/Global/Foo.dll` and
  `Plugins/GameCore/Foo.dll` are two different things, each keeping its own;
- **Different file names do not affect each other**. `Foo.dll` and `FooEx.dll` are two groups;
- **Version numbers are compared as four parts `a.b.c.d`**, `1.10.0.0 > 1.9.0.0` (numerically, not as strings);
- **`0.0.0.0` is equivalent to having no version** and will be rejected. Fill in a non-zero version if you want to take part in the election.

> If your plugin is **exclusive** (you do not want it displaced by a higher version shipped by another Mod), raise the version number high enough,
> or simply use a file name nobody else would use.

### 2.3 Config.sql Template (Bootstrap Configuration)

```sql
UPDATE GameCores
SET
    PackageId = 'your-modinfo-GUID',
    DllPrefix = 'GameCore_XP2_Shell'
WHERE
    GameCore = 'Expansion2';
```
> The Loader core functionality currently targets the **Gathering Storm (EXPANSION_2)** ruleset.

---

## 3. Plugin Categories and Lifecycle Model

The Loader clearly distinguishes two plugin categories, which have completely different lifecycles and call permissions:

| Plugin Category | Directory | Preprocessor Macro | Lifecycle | Use Cases |
|---------|---------|---------|---------|---------|
| **Global plugin** | `Plugins/Global/` | `CIV6_PLUGIN_GLOBAL` | Loaded early in game startup (Phase A), **resident in the process, never unloaded** | Hooking exe-domain logic, resident UI systems, intercepting Windows APIs, global general-purpose tools, etc. |
| **GameCore plugin** | `Plugins/GameCore/` | `CIV6_PLUGIN_GAMECORE` | Loaded on each entry into a match (Phase B), **unloaded when leaving the match / returning to the main menu / reloading the match** | Modifying core gameplay rules, extending GameCore Lua objects, hooking GameCore internal logic, etc. |

### Plugin Export Function Specification

```cpp
#include "HookAPI.h"

// Must export: plugin initialization entry. Returns 0 = initialization failed, non-zero = success
extern "C" __declspec(dllexport) int OnLoad(HookContext* ctx);

// Optional export: cross-plugin message reception
extern "C" __declspec(dllexport) void OnMessage(const char* topic, const void* data, size_t len);

// Optional export: historical legacy (usually does not need to be implemented; just do not export it or keep an empty implementation)
extern "C" __declspec(dllexport) void OnUnload();
```

### Obtaining the Interface Table

What `OnLoad` receives is a `HookContext*`, not the interface table. Two lines turn it into a table you can call directly:

```cpp
static HookAPI* g_api = nullptr;   // store it if you use it across functions; you may also skip storing it if used only once

extern "C" __declspec(dllexport) int OnLoad(HookContext* ctx) {
    if (!HookContext_IsCompatible(ctx)) return 0;    // ABI defense

    HookAPI* api = HookAPI_Resolve(ctx);             // resolve this module's table by name
    if (!HookAPI_RequireAll(api, &HookAPI::Log,      // declare the entries you actually depend on
                            &HookAPI::ResolveByName)) {
        return 0;                                    // missing entry: Loader version mismatch, refuse to load
    }
    g_api = api;

    // call as usual afterwards
    api->Log("my plugin loaded");
    void* target = api->ResolveByName(kHookImageGameCoreXP2, "Some::Symbol");
    return 1;                                        // initialization succeeded
}
```

Key points:

- **`HookAPI_Resolve` is idempotent**: internally it is a function-local `static`; the first call looks up all names and fills the table,
  and subsequent calls return the same pointer directly. Multiple `.cpp` files of the same plugin share this one copy (`inline` function + COMDAT merging),
  so you can call it safely in every file without each creating its own table;
- **`HookAPI_RequireAll` lists only the entries you use yourself**. `HookAPI_Resolve` fills in every cell;
  names that cannot be found are `nullptr` (never a wild pointer), but calling them without checking crashes on a null pointer.
  `RequireAll` lets you detect a missing entry inside `OnLoad` and exit cleanly (`return 0`) instead of blowing up in some callback;
- Parameters use member pointers (`&HookAPI::ResolveByName`) rather than strings, so a misspelled name is a **compile error**,
  the IDE can autocomplete it, and renames follow along;
- **`OnLoad` returning 0 = clean rejection**: the Loader rolls back everything already registered in this load (hooks, captures,
  script globals, messages), **does not activate and does not register `OnMessage`**, and marks the plugin as unavailable (no retry within this process).
  So "exit when an interface is missing" must be written as `return 0`, not merely `return;`—the latter, in the old-style `void OnLoad`,
  amounts to nothing happening and the plugin will still be activated.

### Lifecycle Rules and OnUnload

1. **Idempotency requirements of `OnLoad`**:
   - At the very beginning of `OnLoad` you must first call `HookContext_IsCompatible(ctx)` for ABI version defense.
   - Global plugins are resident in the process, but their `OnLoad` should still guarantee idempotency; do not assume it runs exactly once in its lifetime.
2. **`OnUnload` (historical legacy, no need to implement)**:
   - The Loader already takes over all framework-level resource cleanup fully automatically: when unloading a plugin it automatically retires MinHook detour slots, revokes parameter capture subscriptions, and unregisters message bus and callback listeners.
   - When returning to the main menu or reloading a match, the entire GameCore module and its memory space are completely freed and reclaimed by the game engine, and the next match loads a brand-new official image.
   - Therefore, **plugin authors usually do not need to implement `OnUnload` at all (simply do not export it or keep an empty implementation)**. Never do complex memory destruction in `OnUnload` or `DllMain(DLL_PROCESS_DETACH)`, to avoid conflicting with the host engine's unload order.

---

## 4. Plugin Project Configuration and Build Requirements

- **Build platform**: `x64`; the `Release` configuration is recommended.
- **Runtime library**: statically linking the C/C++ runtime (`Multi-threaded /MT`) is recommended, to avoid the target machine lacking the VC runtime DLLs.
- **Header dependency**: the project only needs to include `HookAPI.h` (self-contained, no dependency on other headers).
- **Version resource (required)**: the project needs a `Version.rc`, and
  `<ResourceCompile Include="Version.rc" />` must be added to the `.vcxproj`. **DLLs without a `VERSIONINFO` file version will not be loaded**,
  and same-named plugins are also ranked by this version number. See §2.2 for the template and rules.
- **Mandatory preprocessor macro definitions (important!)**:
  Under the project properties "C/C++ -> Preprocessor -> Preprocessor Definitions", you **must** explicitly declare one of the following two macros (choose one):
  - Global plugins add: `CIV6_PLUGIN_GLOBAL`
  - GameCore plugins add: `CIV6_PLUGIN_GAMECORE`

> **Category check mechanism**:
> 1. Based on this macro, `HookAPI.h` automatically generates the `Civ6PluginKind` export function at compile time (returning category 1 or 2, plus the API version).
> 2. Before calling `OnLoad`, the Loader reads that export and checks it against the physical directory the plugin is in (`Global` or `GameCore`). If the macro declaration does not match the directory, the Loader **refuses to load** before executing any plugin code.
> 3. If no macro is defined, including `HookAPI.h` directly triggers `#error` and blocks compilation.
> 4. For Global plugins, GameCore-specific interfaces (`GetPlayer`, `RegisterGameEvent`, `TriggerGameEvent`, `RegisterGameCoreScriptForGamePlay`, `RegisterGameCoreScriptForUI`, `PushLuaMethods`) **do not generate the corresponding members**; calling them reports
>    `error C2039: "GetPlayer": is not a member of "HookAPI"`, preventing illegal cross-lifecycle calls.
>
> **Behavior on version mismatch**: `HookContext_IsCompatible(ctx)` returns 0. It compares `structSize` and `apiVersion`—if the plugin and the Loader are not from the same build (the header was changed but only one side was rebuilt),
> the two sides interpret `HookContext` field offsets differently; this check fails before any function pointer is called, so no misaligned call ever happens.

---

## 5. Address Resolution and Cross-Version Compatibility (Key Section)

> This chapter is the longest one in this document. If you are writing a plugin for the first time, you can jump to Chapter 6, get a minimal runnable plugin working, and then come back to read this chapter — but as long as your plugin needs to hook game functions or read/write internal objects, this material is unavoidable.

Every Civilization 6 update (hotfix, launcher change, platform update, etc.) shifts the layout of the code section (`.text`) and the data section (`.data`) of the PE binary. Hardcoding absolute RVAs, or dereferencing structs layer by layer along hardcoded offsets, will break immediately — or even crash — after an update.

The Loader ships a cross-version alignment mechanism based on the PE unwind table (`.pdata`) (`PdataMatch`), and the symbol DB covers **ExeDx11, ExeDx12 and GameCoreXP2**. On top of that, `HookAPI.h` provides a set of addressing wrappers, so plugins do not need to handle version differences themselves.

Reading this chapter in the following order costs the least effort: **§5.2 `HookInfo`** is the core form; using it directly covers the vast majority of needs (the AOB syntax in §5.1 is its fallback — come back to check it when you hit multiple matches). §5.3–§5.6 target specific kinds of goals: the two exe builds, global data slots, vtables, and internal objects that cannot be located by symbol. **§5.7 is an optional availability check**; skipping it does not affect plugin writing; **§5.8 is the tool for looking up a symbol name** (use it first when all you have is an RVA). **§5.9 discusses the applicable scope of function prologue checks** — if you plan to use one to confirm the target function, read it before deciding.

### 5.1 AOB Signature Syntax

AOB (Array of Bytes) is a basic locating mechanism used in many places in this document; the syntax is defined by `ResolveByPattern`, and `HookInfo.aob` and `ResolveDataByAob` all share the same rule set.

**Basic form**: write each byte as two hex digits; bytes may be separated by spaces, tabs, or commas, or written directly adjacent (`488B??` is equivalent to `48 8B ??`).

| Form | Meaning | Example |
|------|------|------|
| `48` | that byte must be exactly `0x48` | `48 89 5C 24` |
| `??` | that byte may take any value (whole-byte wildcard) | `48 89 ?? ??` |
| `4?` | the high nibble is fixed to `4`, the low nibble is arbitrary | matches `0x40`–`0x4F` |
| `?F` | the low nibble is fixed to `F`, the high nibble is arbitrary | matches `0x0F`, `0x1F` … `0xFF` (all 16 values whose low nibble is `F`) |

Hex letters may be upper or lower case. **A wildcard may only appear at a nibble position**: `?` must come in pairs (`??`) or be paired with one hex character (`4?`), and every byte must be written with both nibbles.

If a byte has only **one** nibble written (such as `4` or `?5?`) it usually fails to parse and returns `nullptr` — **but there is one exception to watch out for**: if that extra half follows a hex character immediately (with no space in between), it is swallowed as the first nibble of the next byte, **with no error and no nullptr**:

| Written | Result |
|---|---|
| `4` / `?5?` / `48 8B 4` | parse failure (returns `nullptr`) |
| `?5? 48 8B` | parse failure (returns `nullptr`) |
| **`?5?4 8B`** | **parsed as `?5 ?4 8B` — the `?5?` you thought you wrote became `?5` plus `?4`** |

The last row is a silent mismatch: no error, but it matches a different pattern. So separators between bytes can be omitted (`488B??` is equivalent to `48 8B ??`), but **never leave any byte with only one nibble written**.

```cpp
// Search for the first hit in the .text section (section can be swapped for kSectionImage / kSectionRdata etc.):
void* p = ResolveByPattern<void*>(api, kHookImageGameCoreXP2,
    "48 8B ?? ?? 74 ?? E8", kSectionText);

// When there are multiple hits, use index to pick which one in ascending order (1-based, default 1):
void* p2 = ResolveByPattern<void*>(api, kHookImageGameCoreXP2,
    "48 83 EC 28 8B 15 ?? ?? ?? ??", kSectionText, 2);
```

**Two usage notes**:

1. **The shorter the AOB, the easier it collides**. By default only the **first** hit is taken, so an over-short signature string can very likely hit somewhere else first, resolving to completely unrelated code. It is recommended to copy the whole run of instructions you actually see (usually 15–30 bytes) and wildcard only the displacement and immediate positions that really change between versions.
2. **The wildcarded range determines its resilience to updates**. If you wildcard the call target in `E8 ?? ?? ?? ??` but hardcode the immediately following `48 89 87 30 02 00 00` (a struct offset), then as soon as the struct layout changes this AOB stops working — **wildcard what should be wildcarded, all the way**.

When a signature really cannot be extended to uniqueness (for example several function bodies are byte-identical and differ only in a wildcarded `disp32`), use `index` to disambiguate by position in the ascending hit table. The same `index` can also be written directly into `HookInfo`:

```cpp
// Symbol name as the primary path; take the 2nd ascending hit when falling back to AOB:
constexpr HookInfo kMyFunc = HookName("GameCore::Some::Func", "48 83 EC 28 8B 15 ?? ?? ?? ??", 2);
// Or combined with an RVA primary path:
constexpr HookInfo kMyFunc2 = HookRva(0x100F40, "48 83 EC 28 8B 15 ?? ?? ?? ??", 2);
```

> `index` counts by ascending address **within the same section only**. If the hit count is less than `index`, it returns `nullptr` and logs (the log prints the actual hit count); it never reads out of bounds.

**RIP-relative addressing-specific syntax**: used for `ResolveDataByAob` (see §5.4); it adds two rules on top of the ordinary syntax:

- use `|` to split the signature string into instruction segments, **the segment containing `xx` is the target instruction**;
- `xx` marks the position of the 4-byte `disp32`. Within one signature string **exactly one group of four consecutive `xx`** must appear, and it must fall inside the same segment, otherwise parsing fails.

```cpp
// mov rcx, [rip+disp32]  |  call ...  |  mov [rdi+230h], rax
constexpr char kAob_UIManager[] = "48 8B 0D xx xx xx xx | E8 ?? ?? ?? ?? 48 89 87 30 02 00 00";
```

At runtime the absolute address is computed as `Hit + offset of the target segment end + disp32` (that is, the global slot read by `mov rcx, [rip+disp32]`).

### 5.2 Unified Multi-Level Addressing Model: `HookInfo` (primary path + automatic AOB fallback)

It is recommended to use the `HookInfo` struct to define your hook target. It packs the "primary path (RVA/symbol name/vtable)" and the "backup path (AOB signature fallback)" into one:

```cpp
// Form A: RVA as the primary path, with an AOB fallback
constexpr HookInfo kMyFunc = HookRva(0x64B400, "48 89 5C 24 ?? 57 48 83 EC 20");

// Form B: symbol name as the primary path, with an AOB fallback
constexpr HookInfo kMyFuncByName = HookName(
    "GameCore::Quests::Manager::AttemptNewQuests",
    "48 89 5C 24 ?? 57 48 83 EC 20"
);

// Resolution call:
void* target = ResolveByHookInfo(api, kHookImageGameCoreXP2, kMyFunc);
```

**How it works**:

1. **PdataMatch first**: the Loader takes the old RVA or symbol name and uses the embedded unwind-table signature, prologue hash and monotonic chain to perform cross-version alignment, recovering the current address.
2. **Downgrade on failure**: if alignment cannot be confirmed, `ResolveByHookInfo` automatically falls back to AOB signature scanning (underneath it is the same `ResolveByPattern`, and it honors `HookInfo.index`).

### 5.3 Exe Domain Dual-Version Adaptation: `HookInfoExe`

Civilization 6 has two independent builds, DX11 (`CivilizationVI.exe`) and DX12 (`CivilizationVI_DX12.exe`), with different RVAs and instruction layouts. `HookInfoExe` lets a Global plugin adapt to both at once:

```cpp
// Configure the corresponding RVA and AOB for DX11 and DX12 separately:
constexpr HookInfoExe kExeLoadDLL = {
    HookRva(0xF8510, "48 81 EC 48 02 00 00 48 83 3D ?? ?? ?? ?? 00"),  // DX11 configuration
    HookRva(0xF8850, "48 81 EC 48 02 00 00 48 83 3D ?? ?? ?? ?? 00")   // DX12 configuration
};

// Automatically resolve the matching configuration for the current process (DX11 or DX12):
void* target = ResolveByHookInfoExe(api, kExeLoadDLL);
```
`ResolveByHookInfoExe` probes the exe image of the current process and selects the matching branch; the plugin does not need to write that judgement itself.

### 5.4 Locating Global Data and Singleton Slots: `ResolveDataByAob` (RIP-relative addressing)

Do not copy the absolute RVA of the `.data` section directly — the `.data` section is extremely sensitive to variables being added or removed, and after any update that address is bound to fail.

Code in the `.text` section usually accesses global variables via RIP-relative addressing (such as `mov rcx, [rip + disp32]`). `ResolveDataByAob` extracts the current real data slot from such instructions:

```cpp
// Syntax see 5.1: use | to split instruction segments, xx marks the position of the 4-byte disp32 offset
constexpr char kAob_UIManager[] = "48 8B 0D xx xx xx xx | E8 ?? ?? ?? ?? 48 89 87 30 02 00 00";

// What is resolved is the memory slot address where that global pointer lives (void**)
void* slot = ResolveDataByAob(api, kHookImageExeDx11, kAob_UIManager);
if (slot) {
    void* pUIManager = *reinterpret_cast<void**>(slot);
}
```
It reads the relative displacement inside the instruction, so it is immune to changes of the `.data` section base address or layout.

### 5.5 Virtual Function Slot Symbol Addressing: `ResolveByVTable` / `HookVTable`

If you hardcode an offset directly against a virtual function pointer (such as `vtable[3]`), it will fail as soon as the vtable structure is slightly adjusted. `ResolveByVTable` resolves by slot index together with the symbol DB:

```cpp
// Resolve by vtable symbol name and slot index (0-based):
void* pVFunc = api->ResolveByVTable(
    kHookImageGameCoreXP2,
    "GameCore::Quests::Manager::`vftable'",
    2  // slot index
);

// It can also be wrapped directly into HookInfo:
constexpr HookInfo kMyVTableHook = HookVTable(
    "GameCore::Quests::Manager::`vftable'",
    2,
    "48 83 EC 28 ..."  // optional AOB fallback
);
```

### 5.6 Obtaining Internal Global Objects: `RegisterGlobalCapture`

**Purpose**: to get those global objects/singletons that have no exported symbol and cannot be located with `ResolveBy*` (managers, contexts, world state, etc.). `ResolveByRva` / `ResolveByName` can only give function addresses or global slots of known symbols, whereas many key objects in the game exist only as bare pointers — `RegisterGlobalCapture` is the means to intercept them from function arguments.

Do not dereference layer by layer with hardcoded offsets like `*(void**)((char*)ptr + 0x1A8)`: once the struct is adjusted, all offsets are misplaced, and it usually corrupts memory silently.

The correct approach is to find a function that receives that object as an argument, and use `RegisterGlobalCapture` to intercept the register-passed argument (only the first 4 register arguments `rcx`, `rdx`, `r8`, `r9`, corresponding to paramIndex 0~3). These are **integer arguments**: floating-point/vector arguments go through xmm0–3 and are outside the capturable range.

```cpp
static void* g_pCapturedManager = nullptr;

// Find a function entry that receives that Manager as an argument (e.g. void Init(..., Manager* mgr)):
void* providerFunc = ResolveByName<void*>(api, kHookImageGameCoreXP2, "GameCore::SomeInitFunction");
if (providerFunc) {
    // Capture the 2nd argument (rdx, paramIndex=1) and write it into g_pCapturedManager
    api->RegisterGlobalCapture(providerFunc, 1, &g_pCapturedManager);
}
```
The Loader uses dynamic assembly (a JIT stub) to intercept that argument at the function entry and write it into your pointer slot. As long as the argument-passing semantics of that function do not change, you still get the correct pointer no matter how the object's internal structure changes.

What the stub does is a **traceless relay**: the argument registers seen by the hooked function (`rcx`/`rdx`/`r8`/`r9` and **`xmm0`–`xmm3`**), `rsp`, and the stack contents are exactly as if nothing were hooked. Although the slow path has to call a C function (`CaptureDispatch`) once, it fully saves and restores those four vector registers around the call — they are where the first four floating-point arguments are passed. Only non-argument registers such as `rax`/`r10`/`r11` get clobbered, and by the calling convention a function entry should not rely on their values in the first place.

The capture write happens **before** the target function is called, so what you get in `*out` is the argument **passed by this call**; before the provider is called for the first time, it is still the `nullptr` you initialized. Subscriptions are **append-only**: multiple plugins' `out` pointers can be attached to the same address, a later registration does not displace an earlier one, and on trigger they are all written.

**Early revocation: `UnregisterGlobalCapture(providerAddress, out)`**. The match condition is that all three — (provider, out, your plugin) — are identical; you cannot revoke someone else's slot, and wrong arguments simply return `false` and log. It **removes only the subscription, not the hook**: the site and stub are never released, and once the last subscription is removed that address immediately returns to the fast path with no capture. On plugin unload/quarantine the Loader automatically clears the remaining subscriptions by owner, so this is an optional early-release mechanism (for example when you only need that pointer during a certain stage).

> **Division of labor with `ResolveDataByAob` (§5.4)**: the latter resolves global slots that are directly referenced by `mov reg, [rip+disp32]` in code, suitable for singletons referenced by a fixed instruction; this section captures via function arguments, suitable for objects that are only passed in during initialization and have no fixed reference point in code. Neither depends on struct offsets.

### 5.7 Availability Check (Optional): `GetSymbolDbState`

The forms in the previous sections already handle version differences automatically — if the primary path of `HookInfo` fails it falls back to AOB, and `ResolveByName` / `ResolveByRva` are also corrected through `PdataMatch`. **The vast majority of plugins do not need to care about this section**; just write as in §5.2.

The two interfaces introduced in this section provide a **state snapshot**: they tell you how much of the embedded symbol DB matches the current image. This is useful in two scenarios:

- **You need to distinguish the reason for "not found"**. When resolution returns `nullptr`, is it that "this function really does not exist in this version", or that "the symbol DB is simply unavailable"? For the former you should turn the feature off; for the latter it is more appropriate to prompt the user to update the Loader. Without this interface all you can see in the log is `no symbol`.
- **The feature is very sensitive to failure and is better left off**. For example, a certain hook would corrupt save files once it resolves to the wrong place, while the AOB fallback carries a collision risk — in that case you can require the symbol DB to be `Identical`, otherwise the whole feature stays disabled.

```cpp
// Call only when you need the above judgement; normal resolution does not need it.
const HookApiSymbolDbState st = api->GetSymbolDbState(kHookImageGameCoreXP2);

void* fn = ResolveByHookInfo(api, kHookImageGameCoreXP2, kMyFunc);   // primary path unchanged
if (!fn) {
    if (st == kHookApiDbNoContext || st == kHookApiDbBuildFailed) {
        // The symbol DB is entirely unavailable: not "the function does not exist" but "we cannot look it up"
        api->Log("symbol db unavailable (%d); ask user to update loader", static_cast<int>(st));
    } else {
        api->Log("target not found in this game version; feature disabled");
    }
}
```

Meaning of each state:

| State | Meaning |
|---|---|
| `kHookApiDbIdentical` | the symbol DB and the current image are **the same build**; RVAs are identical |
| `kHookApiDbCrossVersion` | cross-version alignment succeeded; old RVAs get corrected |
| `kHookApiDbNotReady` | symbol data for this image exists, but the table is not built yet (too early; re-check later) |
| `kHookApiDbBuildFailed` | table building failed; no correction in this session |
| `kHookApiDbNoContext` | the sidecar has no module for this image at all |

**`kHookApiDbBuildFailed` / `kHookApiDbNotReady`** (symbol data for this image exists, but the table was not built): `ResolveByRva` and `ResolveByName` both return `nullptr`, fail-closed; they will not guess from an old RVA.

**`kHookApiDbNoContext`** (the sidecar has no such image at all): `ResolveByName` returns `nullptr` (no symbol to look up); but `ResolveByRva` **treats the RVA you give it directly as the current coordinate** — because the Loader has no basis for correction and can only pass it through as-is. That may happen to be correct, or it may land on the wrong function. When you see this state, stop using old RVAs and switch to `ResolveByPattern` with an AOB, or disable the feature.

> Neither of these "unavailable" states means you should fall back to bare-RVA interfaces such as `ResolveByRvaEx` — when the symbol DB is unavailable, an old RVA has no basis at all.

**`GetSymbolDbMatchRatio(image)`** returns the cross-version alignment match ratio (aligned `.pdata` entries ÷ total), and is a measure of "partial match" only under `kHookApiDbCrossVersion`; `Identical` returns `1.0`, everything else returns `0.0`. Good for writing into logs to help with remote troubleshooting.

> These two interfaces only give a judgement; they do not change resolution behavior — they cannot replace the multi-path locating of `HookInfo`. **Whether a single point can be resolved always depends on the return value of `ResolveBy*`**: however high the overall match ratio is, it may still happen to miss the one function you want.
>
> `image` takes `kHookImageExeDx11` / `kHookImageExeDx12` / `kHookImageGameCoreXP2`; the states of different images are independent (for example the exe may be `Identical` while GameCore is `CrossVersion`).

### 5.8 Looking Up an RVA as a Symbol Name: `tools/rva_to_symbol`

The repository ships `tools/rva_to_symbol.exe` (single file, the symbol table is embedded at compile time, no Rust environment needed):
This tool takes an RVA and returns the corresponding symbol from the symbol DB embedded in the Loader.
**Note: decompilation cannot recover all symbols, so it is normal for a symbol name to be inconsistent with what the function actually does. As long as each RVA has a unique corresponding symbol, that is enough; this symbol DB will most likely never be updated again in the future, so wrong symbols carry no risk of resolution errors (except for official updates). Of course, if you think wrong symbols would make reading difficult, you can also just use the RVA.**

```text
tools\rva_to_symbol.exe -gamecore 0x5BDAC0
tools\rva_to_symbol.exe -dx11 0x4D0D70
tools\rva_to_symbol.exe -dx12 0x37AB0 0x37DE0
tools\rva_to_symbol.exe --list
```

| Selector | Image | Corresponding `HookImageId` |
|---|---|---|
| `-gamecore` | `GameCore_XP2_FinalRelease.dll` | `kHookImageGameCoreXP2` |
| `-dx11` | `CivilizationVI.exe` | `kHookImageExeDx11` |
| `-dx12` | `CivilizationVI_DX12.exe` | `kHookImageExeDx12` |

**The input is an RVA, not an absolute address**. When what you have in IDA/x64dbg is an absolute address, subtract the image base `0x140000000` first:
`0x1404D0D70 - 0x140000000 = 0x4D0D70`. The RVA spaces of the three images are mutually exclusive, so the selector must be given explicitly —
the tool will not give you a "default image".

The output always has four columns: `kind` / `demangled` / `mangled` / `vtable_xrefs`, and you can also use something like `--demangled` to get only one column;
if nothing is found it prints `0x...: not found` (exit code 1). Typical usage:

```text
# You only want the name, to make ResolveByName easier to write:
tools\rva_to_symbol.exe -gamecore 0x5BDAC0 --demangled
#   -> GameCore::Lua::Utility::RegisterScriptData

# pRegister for dx11 (the engine entry point that registers the Modding script library):
tools\rva_to_symbol.exe -dx11 0x4D0D70 --demangled
#   -> AppLuaModdingLibrary::pRegister

# Look up several RVAs at once:
tools\rva_to_symbol.exe -dx12 0x37AB0 0x37DE0 --demangled
```

Once you have the symbol name, write it as `HookName("GameCore::…")` per §5.2 and leave cross-version correction to `PdataMatch`; prefer the demangled symbol, since that is what achieves readability, and it is recommended to write the RVA in a comment for later verification.

> Symbol names come from the IDA symbol library and are a snapshot of **one particular analysis**, not an official game export. DX12's IDB symbol coverage is sparser than DX11's.

### 5.9 Function Prologue Check: **Use With Caution**

> **Terminology note**: "function prologue" in this document means the first few instructions at a function entry, usually used to set up the stack frame and save registers. Older versions of the documentation called it "leading bytes", which is not the standard translation; it has now been unified as "function prologue".

Some plugins, after resolving an address, compare the machine code at the target function's prologue (for example "the first 8 bytes must be `48 89 5C 24 08`"), and if it does not match they conclude "the game was updated" and give up on the feature.

**This practice is not unusable, but you must be clear about its failure conditions.** This project has now removed it from all modules, for the following reasons.

#### Main Risk: The Function Prologue Can Be Rewritten by Other Plugins

A function prologue is not a stable identifier. Anyone who has ever hooked that function rewrites the entry into a jump form via MinHook:

```asm
; original prologue
48 89 5C 24 08    mov [rsp+8], rbx

; after being hooked
E9 xx xx xx xx    jmp <trampoline>     ; the first 5 bytes have been replaced
```

As a result, **a plugin loaded later does not read the original bytes at all**, and the check is bound to fail.

#### Knock-On Effect: Cascading Failure That Depends on Order

A real incident: one module in this project first installed a hook on `AddGreatWorkToSlot`, and another module then performed a prologue check, read the rewritten entry → judged `mismatch` → that module marked the whole group of symbols as "incomplete" → all Lua APIs collectively returned `false`.

The symptom looks like "the game was updated", but it is actually **plugins interfering with each other**, and **whether you pass the check depends on load order** — this kind of implicit coupling is extremely hard to troubleshoot.

#### What Is Worse: The Failure Reason Cannot Be Distinguished

The original intent of a prologue check is "cross-version alignment of the symbol DB may map the name onto the wrong function". But there are two possibilities when it fails, and **the symptom is exactly the same** (both are `mismatch`):

| Actual situation | Behavior of the prologue check | Should the feature be given up |
|---|---|---|
| the symbol alignment is wrong and points to another function | mismatch | ✅ should give up |
| the symbol alignment is correct, but the entry was hooked by someone else | mismatch | ❌ should not give up |

You cannot tell from `mismatch` itself which case you are in. If you therefore turn off the entire feature, you are using a check that necessarily kills valid cases to guard against a low-probability problem.

#### When It Can Be Used With Caution

A prologue check is not entirely worthless. When **all** of the following conditions are met, it is reasonable:

1. **The function will not become a hook target of other plugins** (for example a leaf function that only accesses data, a `retn` wrapper);
2. **The check happens early enough** — in `OnLoad`, and you are sure nobody has modified it before that;
3. **Failure only degrades, never cascades** — when the prologue does not match, disable only this one feature; do not treat it as evidence that "the symbol DB is broken" and disable the whole group;
4. **The signature is long and stable enough**, and has been confirmed unique within the current IDB (too short and it collides; something like `48 89 5C 24 08` holds on very many functions).

Conversely, as long as the target function **may be hooked by another plugin**, do not use it for judgement.

#### More Reliable Alternatives

| Purpose | Recommended means |
|---|---|
| cross-version locating | the symbol-name primary path of `HookInfo` (`PdataMatch` alignment of the Loader) |
| fallback | an AOB signature, and you must confirm it hits **uniquely within the current IDB** |
| confirm the target is "alive" | do a lightweight runtime self-check after the call (return value range, null pointer, enum validity) |
| judge whether the symbol DB is available | `GetSymbolDbState` (see §5.7), which gives **global state** rather than single-point byte comparison |

#### Current State of the Interfaces in This Project

`gc::ResolveVerified<T>(api, image, info, sig, sigLen, label)` in `Common/GameCoreResolve.h` still keeps the two parameters `sig` / `sigLen`, purely so as **not to change the calling code of each module** — they are now completely unused, and the function only does "resolve + log on failure".

New code should call directly:

```cpp
void* fn = gc::Resolve<void*>(api, kHookImageGameCoreXP2, kTargetMyFunc, 16);
if (!fn) {
    api->Log("%s", "[MyFeature] target unresolved - skipped");
    return;
}
```

`gc::PrologueMatches` / `gc::PrologueMatchesMasked` / `gc::ParseAob` / `gc::ResolveVerifiedAob` have been removed from the codebase. If a feature really does need a prologue check, add it back yourself according to the conditions in "When It Can Be Used With Caution" above, and make sure failure only degrades and never cascades.
---

## 6. HookAPI Core Features and Usage Examples

This chapter is grouped by feature: §6.1 is the initialization template that every plugin needs; §6.2–§6.3 are the two most common ways to change game logic (hooking a function and changing bytes); §6.4–§6.8 are the Lua and utility interfaces; **§6.9 is log leveling (hot-path logs must be gated)**; **§6.10 is advanced material** — you need to read it only when a plain C++ detour cannot express your need.

### 6.1 Basic Entry Point and Compatibility Defense

```cpp
static HookAPI* g_api = nullptr;

extern "C" __declspec(dllexport) int OnLoad(HookContext* ctx) {
    // ABI compatibility must be checked first; return 0 = initialization failed
    if (!HookContext_IsCompatible(ctx)) {
        return 0;
    }

    HookAPI* api = HookAPI_Resolve(ctx);
    // Only list the entries this plugin truly depends on; one missing means the Loader version does not match — exit cleanly
    if (!HookAPI_RequireAll(api, &HookAPI::Log)) {
        return 0;
    }
    g_api = api;

    api->Log("MyPlugin: OnLoad successfully initialized.");
    return 1;   // initialization succeeded
}
```

### 6.2 Installing a Function Hook (MinHook)

```cpp
using AttemptNewQuestsFn = void(__fastcall*)(void* thisPtr);
static AttemptNewQuestsFn g_origAttemptNewQuests = nullptr;

void __fastcall MyAttemptNewQuests(void* thisPtr) {
    // run pre logic
    g_origAttemptNewQuests(thisPtr);
    // run post logic
}

// install in OnLoad:
void* target = ResolveByName<void*>(api, kHookImageGameCoreXP2, "GameCore::Quests::Manager::AttemptNewQuests");
if (target) {
    InstallHook(api, target, reinterpret_cast<void*>(&MyAttemptNewQuests), &g_origAttemptNewQuests);
}
```

**The detour must be a real function inside the plugin DLL.** The Loader verifies module ownership of the detour address; addresses produced at runtime by `VirtualAlloc` / JIT are rejected. If you really need machine code as the detour (rewriting registers, jumping back into the middle of a function), see §6.9.

### 6.3 Byte-Level Memory Patching (Patch / Unpatch)

`Patch` writes hardcoded machine code into game memory (for example nop-ing out a conditional jump, or changing an immediate), for those scenarios where **installing a hook is not suitable**:

```cpp
static uint8_t s_nopBytes[] = { 0x90, 0x90 };
static void* s_patchTarget = nullptr;

// apply the patch in OnLoad:
s_patchTarget = ResolveByHookInfo(api, kHookImageGameCoreXP2,
    HookName("GameCore::Some::Func", "48 83 EC 28 ?? ?? ?? ??"));
if (s_patchTarget) {
    api->Patch(s_patchTarget, s_nopBytes, sizeof(s_nopBytes));
}
```

**Usage conventions**:

1. **The same address can be patched only once by the same plugin.** Patching the same target again is rejected outright — the patch is already in effect, and stacking another layer makes it impossible to track which layer belongs to whom. To change bytes, `Unpatch` first, then `Patch`.
2. **A site that is already hooked cannot be patched.** The Loader forbids this combination in both directions (MinHook rewrites the target prologue, and both sides keeping their own copy of the "original bytes" is bound to overwrite each other): if there is a hook inside the target's 32-byte leading window → `Patch` is rejected; conversely, if a range already carries a patch → `InstallHook` is rejected. To change an already hooked function, implement it inside the detour.
3. **Different plugins can stack patches.** Each target maintains an owner stack in application order, and each layer stores the bytes "as they were before it wrote". Therefore `Unpatch` restores the state you saw when you applied your patch, and will not clobber a patch someone else applied later.
4. **Design on the basis of "effective and never restored".** Treat the patch as continuously effective for the lifetime of the process; do not revoke it on unload or when the match changes. The Loader itself performs no automatic rollback.
5. **If a hook can do the job, do not patch.** A byte patch is bound to the game version you copied it from; after an update, once the instruction at that place changes, what you write becomes garbage. `InstallHook` uses dual `PdataMatch` + AOB address resolution and can follow updates.
6. **Use function prologue validation with caution.** See §5.9: a function entry gets rewritten by a hook into `E9 xx xx xx xx`, at which point "comparing the original entry bytes" necessarily fails, and the failure cause cannot be distinguished from "the game was updated". Unless you are sure that no one else hooks that function, do not use it as a resolution criterion.

**Use `Unpatch` with caution.** Its legitimate scenario is dynamically toggling a change at runtime (for example a config option shipped with the plugin that must take effect immediately). Before using it, note:

- **Only the top layer of the stack can be withdrawn**: if another plugin patched the same address after you, your `Unpatch` is rejected and you have to wait for it to withdraw first. Writing old bytes past the upper layer would silently overwrite its patch, so the Loader does not allow it;
- **Only the plugin that originally applied the patch can restore it**, and the call must happen while that plugin is **still alive** to have any chance of success. Once a plugin is quarantined by the Loader because of a crash (after which none of its code ever runs again) or its module is unloaded, there is no further opportunity;
- Restoration writes memory over code that **may be executing right now**, with no quiesce fallback, so do not put it on the unload path as routine cleanup.

**Conclusion**: `Patch` is the normal means of modifying the instructions themselves; `Unpatch` is reserved for scenarios that genuinely need dynamic restoration — do not treat it as a routine step of unload cleanup.

### 6.4 Getting the Global Main Lua VM and Lua Stack Operations

The Loader provides direct access to the engine's process-level main Lua VM (`m_pkMainLuaState`), **safely callable from both Global and GameCore plugins** (operation from the game main thread only):

```cpp
void* L = api->GetMainLuaState();
if (L) {
    api->LuaDoString(L, "print('Hello from C++ plugin via Main Lua State!')");
}
```

There is also a complete set of Lua stack and table operation wrappers (type checking, pushing, reading values, table read/write, `pcall`, etc.), with the same names and meanings as the standard Lua 5.1 C API. See appendix §A.4 for the full list.

### 6.5 Extending GameCore Lua Object Methods (PushLuaMethods, GameCore only)

Attaching custom methods to the game engine's existing C++ Lua export objects (such as `IQuestsManager`):

```cpp
static int __fastcall lCustomMethod(void* L) {
    void* pMgr = api->LuaToUserData(L, 1);
    api->LuaPushInteger(L, 42);
    return 1;
}

static const LuaMethodInfo s_methods[] = {
    { lCustomMethod, "lCustomMethod", "CustomMethod" }
};

// attach in OnLoad:
api->PushLuaMethods("GameCore::Lua::IQuestsManager::PushMethods", kHookApiPushNoManager, s_methods, 1);
```
Afterwards it can be called directly from a Lua script: `Players[0]:GetQuestsManager():CustomMethod()`.

A few points that need to be clear:

- **The first argument is the full symbol name**, handed verbatim to symbol resolution; the Loader does no concatenation or inference —
  write exactly what the target function is called;
- **`sigType` must match the actual signature of that PushMethods**:
  `kHookApiPushNoManager` = `PushMethods(lua_State*, int)`;
  `kHookApiPushWithManager` = `PushMethods(Manager*, lua_State*, int)`.
  A wrong value reports no error, but the arguments are all shifted — resolve the address with `tools/rva_to_symbol.exe` and disassemble to confirm the first argument.

### 6.6 Registering the Script Global Environment (three entries, choose by scope)

Injecting custom modules into the Lua global table `_G`. The three entries differ only in **which Lua environment they attach to** and **when they run**:

| Entry | Category | Attaches to | Lifetime |
|---|---|---|---|
| `RegisterGameCoreScriptForGamePlay` | GameCore only | GameCore's gameplay Lua | Rebuilt every match, invalidated when the match unloads |
| `RegisterGameCoreScriptForUI` | GameCore only | GameCore's UI script Lua | Same as above (not the same state as gameplay) |
| `RegisterGlobalScript` | **Global only** | **every** Lua environment created by the exe (`FrontEnd` / `MainMenu` / `Options` / match …) | Process-level; runs once per environment |

The callback signature is uniformly `void (*)(void* L)`, where `L` is the corresponding `lua_State*`, and the Lua C API can be used directly:

```cpp
static int __fastcall lDoSomething(void* L) {
    api->Log("DoSomething called from Lua!");
    return 0;
}

static void InitGlobals(void* L) {
    api->LuaNewGlobalTable(L, "MyMod", 0, 2);
    api->LuaPushCFunctionField(L, -1, "DoSomething", reinterpret_cast<void*>(&lDoSomething), "lDoSomething");
    api->LuaPop(L, 1);
}

// for in-match scripts (GameCore plugin):
api->RegisterGameCoreScriptForGamePlay(&InitGlobals);
// for GameCore-side UI scripts (GameCore plugin):
api->RegisterGameCoreScriptForUI(&InitGlobals);
// for process-level globals (visible to Global plugins only): runs once per Lua environment
api->RegisterGlobalScript(&InitGlobals);
```

A few key points about `RegisterGlobalScript`:

- **It runs once per Lua environment**: the engine creates a separate `lua_State` for each script environment (`FrontEnd`, `MainMenu`,
  `Options`, `CreditsScreen`, the match's gameplay …), each with its **own environment table** — an interface attached to one of
  them is **not accessible** from the others. So the Loader calls your callback once per environment, and `L` is that one environment;
- **It runs only once per state**: repeated triggering of the same environment (for example a UI rebuild going through the same registration path) does not execute it again;
  changing the match or rebuilding the environment (a new state) runs it once more;
- **It executes on the game thread**: inside the callback you can call the Lua C API directly; you do not need to switch threads yourself;
- **`L` is the state of that environment**, not some fixed pointer you obtained beforehand. If you want the interface available in all environments,
  simply attach it to the incoming `L` — the Loader guarantees that every environment gets called; if you want it to take effect only in certain environments, you can decide
  on demand inside the callback (for example by comparing against `GetMainLuaState()`);
- When the plugin is unloaded/quarantined, the Loader automatically removes the registration by owner.

> One easy pitfall: **do not cache the `L` you got the first time and keep writing to it**. Each environment is independent;
> writing to an old `L` only affects that one environment, and the new environment still sees nothing. The correct approach is to attach to the current `L` on every callback.

> ⚠ **Pitfall two: do not raise Lua errors inside the callback.**
> Entries such as `LuaError` / `LuaCheck*` go through longjmp when they fail, **not a normal return**.
> The Loader wraps these callbacks with `__try/__except` as a fallback, but `__try` cannot catch longjmp —
> once one raises, all C++ destructors in the Loader frames are skipped, the in-flight counter never decrements,
> and the later `WaitForDetourDrain` / `WaitAll` on match change or unload **blocks forever** (which shows up as a hang).
>
> The correct form: **use `LuaType` to check the type and simply `return` when it is invalid**, instead of letting Lua throw.
>
> ```cpp
> static int __fastcall lSetLevel(void* L) {
>     // do not write LuaCheckInteger(L, 1) — it raises when the argument is wrong
>     if (api->LuaType(L, 1) != 3) return 0;   // 3 = LUA_TNUMBER, silently ignore
>     const int n = static_cast<int>(api->LuaToNumber(L, 1));
>     // ...
>     return 0;
> }
> ```
>
> That is exactly how `GreatWorkAPI.cpp` was changed (see item 4 in that file's header comment).

### 6.7 Game Custom Event Bus (GameEvent)

Supports triggering a custom GameEvent with arbitrary arguments from the C++ side into Lua (argument packing is done through the `GameEventPushFn` callback; the Loader handles engine lock acquire/release and exception protection):

```cpp
// 1. Define the argument packing callback: push arbitrary arguments onto the stack, return the argument count
static int PushCustomEventArgs(void* L, const void* payload) {
    const int val = *reinterpret_cast<const int*>(payload);
    api->LuaPushInteger(L, val);
    api->LuaPushInteger(L, 100);
    return 2; // 2 arguments were passed to Lua
}

// 2. Register the custom event name
api->RegisterGameEvent("MyModCustomEvent", &PushCustomEventArgs);

// 3. Call it wherever you need to trigger it (for example inside a MinHook detour):
int myPayload = 42;
api->TriggerGameEvent("MyModCustomEvent", &myPayload);
```
The game's Lua side can then listen to it normally:
```lua
GameEvents.MyModCustomEvent.Add(function(param1, param2)
    print("Received custom event:", param1, param2)
end)
```
### 6.8 Localization and Utility Interfaces

- **Localization lookup**:
  ```cpp
  void* loc = api->LocCreate();
  api->LocLookup(loc, "LOC_TECH_POTTERY_NAME");
  const char* text = api->LocToUtf8(loc);
  api->Log("Tech: %s", text ? text : "");
  api->LocDestroy(loc);
  ```
- **Engine-consistent string hashing**:
  ```cpp
  uint32_t hash = api->MakeHash("YIELD_GOLD"); // strictly consistent with GameCore's internal CRC32, usable at any time
  ```
- **Getting a player pointer**:
  ```cpp
  void* pPlayer = api->GetPlayer(0); // get the player 0 pointer (GameCore only)
  ```
- **Developer debug switch query**: `IsDebugLogEnabled()`, see §6.9.

### 6.9 Log Leveling: Gating Hot-Path Logs with `IsDebugLogEnabled`

**This is a hard requirement, not a suggestion.** The Loader and all plugins share the same log file
(`Civ6DllLoader_Loader.log`), and logs on hot paths will flood it.

Measured data: in a 1884-line log, one route refresh callback alone took 653 lines and a module registered once per
Lua state took 426 lines, the two together **57%**. The situation at the time was that the lines you really needed to see, such as load failures and
hooks failing to install, were all buried in the middle.

#### How to Decide Whether a Log Line Should Be Gated

| Nature of the log | Gate or not | Example |
|---|---|---|
| Initialized once, only logged on failure | ❌ No gate, always output | `hooked X at %p`, `registration FAILED`, `symbol missing` |
| Logged on every hook callback | ✅ Must gate | entry/exit logs of every detour body |
| Logged every frame / every refresh / every creation | ✅ Must gate | route refresh, requirement evaluation, factory Create |
| Player-visible state changes | ✅ Must gate | which value was changed, which mode was switched |

In one sentence: **any log whose frequency relates to "every game interaction" must be gated; only logs that "make no sound once installed"
may be emitted bare.**

#### How to Write It

```cpp
HookAPI* g_api = nullptr;

// Hot-path log switch. Ask api directly every time, do not cache a bool yourself —
// caching adds a "when to cache" timing problem (api is not available before OnLoad).
inline bool DebugLogOn() {
    return g_api && g_api->IsDebugLogEnabled && g_api->IsDebugLogEnabled();
}

// Hot path: triggered on every callback
void __fastcall Hook_Something(void* self) {
    if (DebugLogOn()) {
        g_api->Log("MyFeature: Thing called self=%p", self);
    }
    ...
}

// Initialization path: installed once, no gate needed
g_api->Log("MyFeature: Detour_Something installed at %p", g_detour);
```

#### Switch Semantics

- The value comes from `[Loader] ENABLE_DEBUG_LOG` in `<exe directory>\Civ6DllLoader\config.ini`;
- **Defaults to false**, constant within the process (changing the ini requires a game restart to take effect);
- **Read-only, hand-edit only**: no Writer, no UI, no Lua interface, and plugins should not change it either;
- A missing key / an invalid value is always read as `false` (fail-closed).

#### Pitfalls to Avoid

- **Do not use `if (g_api)` instead of `if (DebugLogOn())`**: the former only guards against a null pointer, and the log still floods.
- **Do not put initialization logs in there too**: those lines are only one line anyway, and gating them turns "why was it not printed"
  into a new direction of investigation.
- **Do not do string formatting on hot paths**: `g_api->Log` is varargs, so arguments are evaluated before the call.
  Building the string must go inside the gate, otherwise you pay the formatting cost even when the gate is off.

### 6.10 Generating detours with asmjit (Advanced)

Some hooks cannot use an ordinary C++ function as the detour: you need to rewrite registers before entering the original function, jump back conditionally into the middle of the original function, or squeeze a very short piece of logic into the target function's instruction stream. In that case you can use asmjit to assemble that machine code at runtime.

> **The code in this chapter is written for asmjit 1.17** (the version vendored in this repository, `ASMJIT_LIBRARY_VERSION` = 1.17.0).
> 1.20+ changed several API names (`kErrorOk`→`Error::kOk`, `codeSize()`→`code_size()`,
> `errorAsString()`→`error_as_string()`), and copying it as-is will not compile — see §6.10.9 for the comparison table.

**This is not a required technique.** As long as an ordinary C++ detour can meet the need, use an ordinary detour — one less layer of machine code is one less class of crash.

---

#### 6.10.1 Hard Constraint One: the detour must land inside the plugin's own module

Before installing, `InstallHook` verifies ownership of the **detour address**: it uses `GetModuleHandleExW(FROM_ADDRESS)` to look up which loaded module it belongs to, then confirms that the module is a plugin registered in this process.

Therefore:

| Source of the detour | Result |
|--------------|------|
| An ordinary function in the plugin DLL | ✅ Passes |
| A custom section in the plugin DLL (see below) | ✅ Passes |
| The JIT page returned by `VirtualAlloc` / `JitRuntime::add` | ❌ Rejected, log: `InstallHook rejected: caller/callback is not an active plugin` |
| Heap memory | ❌ Rejected |

**The `fn` returned by asmjit's `rt.add(&fn, &code)` lives on the JIT page and cannot be used directly as a detour.** The correct approach is to treat it as an "encoder": after obtaining the bytes, `memcpy` them into storage inside the plugin module, then `rt.release(fn)`.

> The check only applies to the **detour argument**. If you do not call `InstallHook`, but instead use `Patch` to write a jump into the game's instructions so that control flow jumps to the JIT page by itself, the Loader does not check the jump target — but that is another route, see §6.10.7.

---

#### 6.10.2 Hard Constraint Two: writable pages and executable pages must be separate (W^X)

You need a piece of memory that "can be written and can be executed". **Do not** make it permanently RWX (`PAGE_EXECUTE_READWRITE`, or `/SECTION:.uhook,RWE`):

- Any single out-of-bounds write can turn data directly into executable code;
- A bad write does not crash immediately; it is delayed until some call jumps into the broken instruction, and at that point the call stack has nothing to do with the line that was corrupted, making it extremely hard to locate.

The correct timeline is:

```
load time     → RW    (can be written, cannot be executed)
after emit    → RX    (can be executed, cannot be written)
```

Implemented with a custom section:

```cpp
// Declared readable and writable, 【not executable】
#pragma section(".uhook", read, write)

// C++17 inline: only one definition is kept when multiple TUs include the header
__declspec(allocate(".uhook")) alignas(4096) inline unsigned char g_detourCode[4096];
```

**Do not** write `#pragma section(".uhook", read, execute)` together with `/SECTION:.uhook,RWE` — that makes the section permanently RWX. Also do not give `write` and `execute` at the same time: MSVC reports C4330 and silently drops `write`, so you end up with an R+X section that crashes on the first write at runtime.

Sealing the page:

```cpp
DWORD old = 0;
VirtualProtect(g_detourCode, sizeof(g_detourCode), PAGE_EXECUTE_READ, &old);
FlushInstructionCache(GetCurrentProcess(), g_detourCode, sizeof(g_detourCode));
```

`FlushInstructionCache` is **mandatory**. On x64, not calling it "usually also works", but that is a coincidence, not a guarantee.

---

#### 6.10.3 Hard Constraint Three: every emit error must be checked

This is the easiest pitfall to step into, and it has **no error reporting at all**.

Every asmjit emit call returns `Error`, and on failure that instruction is **not written into the buffer**. The problem is that `JitRuntime::_add()` only checks flattening, relocation, and length, and **does not check whether the emitter ever reported an error** — it still returns success.

So as long as one place does not check the return value, this happens: the hook body is missing an instruction, yet it still gets installed. Behavior is scrambled, and there is not a single word in the log.

Typical triggering scenarios (limitations of x86-64 encoding itself, not asmjit bugs):

| Form | Result |
|------|------|
| `a.cmp(x86::rbx, 0x1_0000_0000ULL)` | ❌ `cmp r/m64, imm64` does not exist, only `imm32` (sign-extended) |
| `a.cmp(x86::rbx, 0x1234)` | ✅ falls within int32, encodes normally |
| `a.mov(x86::rax, 0x1_0000_0000ULL)` | ✅ `mov r64, imm64` (the `B8+` form) exists |

So every single emit must be checked:

```cpp
#define ASMJIT_EMIT(expr)                                                        \
    do {                                                                         \
        asmjit::Error _e = (expr);                                               \
        if (_e != asmjit::kErrorOk) {                                            \
            api->Log("[Hook] asmjit emit failed @%d (%s): %s",                   \
                __LINE__, #expr, asmjit::DebugUtils::errorAsString(_e));         \
            return false;                                                        \
        }                                                                        \
    } while (0)
```

> The names must be `asmjit::kErrorOk` and `asmjit::DebugUtils::errorAsString`. What this repository vendors is
> asmjit **1.17**, where `using Error = uint32_t;` (**not** a class) and error codes live in `enum ErrorCode` —
> writing `asmjit::Error::kOk` / `error_as_string` does not compile under 1.17 (that is the 1.20+ form).
> For a definition in the repository that can be copied directly, see `ASMJIT_EMIT` in `UnlockModelLimit_Common.h`.

> Note that this macro can only be used in functions that return `bool` (the failure path is `return false`). Functions returning `void` need to be expanded separately.

---

#### 6.10.4 Complete Skeleton

```cpp
// What this repository vendors is asmjit 1.17: use asmjit/x86.h (it already includes core.h internally).
// The code below is written against the 1.17 API; for 1.20+ differences see the comparison table in §6.10.9.
#include "asmjit/x86.h"

// One JitRuntime shared across the process is enough; it only handles "allocating a temporary executable page for a CodeHolder".
static asmjit::JitRuntime g_rt;

// ---- emit + write + seal + install ----
inline bool InstallAsmDetour(HookAPI* api, void* target, void** original,
                             asmjit::CodeHolder& code) {
    void* fn = nullptr;
    asmjit::Error err = g_rt.add(&fn, &code);
    if (err != asmjit::kErrorOk || !fn) {
        api->Log("asmjit jit failed: %s", asmjit::DebugUtils::errorAsString(err));
        return false;
    }

    const size_t size = code.codeSize();           // 1.20+ is code.code_size()
    bool ok = false;
    if (size > 0 && size <= sizeof(g_detourCode)) {
        DWORD old = 0;
        if (VirtualProtect(g_detourCode, sizeof(g_detourCode), PAGE_READWRITE, &old)) {
            memcpy(g_detourCode, fn, size);
            if (size < sizeof(g_detourCode)) {
                memset(g_detourCode + size, 0xCC, sizeof(g_detourCode) - size);  // fill the tail with int3
            }
            // must switch to RX before InstallHook: once the entry is rewritten, anyone may jump in at any moment
            if (VirtualProtect(g_detourCode, sizeof(g_detourCode), PAGE_EXECUTE_READ, &old)) {
                FlushInstructionCache(GetCurrentProcess(), g_detourCode, sizeof(g_detourCode));
                ok = api->InstallHook(target, g_detourCode, original);
            }
        }
    } else {
        api->Log("hook code size %zu exceeds slot capacity %zu", size, sizeof(g_detourCode));
    }

    g_rt.release(fn);                 // the bytes have been copied out, return the JIT page immediately
    return ok;
}

// ---- caller (inside OnLoad) ----
asmjit::CodeHolder code;
code.init(g_rt.environment());
asmjit::x86::Assembler a(&code);
ASMJIT_EMIT(a.push(asmjit::x86::rcx));
ASMJIT_EMIT(a.mov(asmjit::x86::rax, (uint64_t)someAbsoluteAddress));  // always use an immediate for absolute addresses
// ...
InstallAsmDetour(api, target, &g_orig, code);
```

Note that the `bool` returned by `InstallAsmDetour` in the skeleton must actually be checked. If it cannot be installed, `OnLoad` should return failure (or at least log a line); do not continue silently — a hook that "looks installed but actually goes to the original function on every call" is harder to diagnose than a crash.

---

> **If this is a Global plugin**: the detour code stays resident with the module until the process exits, so the above "write into a custom section" approach is safe.
> **If this is a GameCore plugin**: the module is really unloaded when returning to the main menu / changing the match. The Loader restores the entry bytes in batches before unloading,
> so under normal circumstances no one will jump into your detour afterwards; but **do not** free or rewrite this block of code early in the plugin's own cleanup path.

---

#### 6.10.5 With Multiple detours: One Page Per Slot

`VirtualProtect` takes effect per memory page (4 KB). If multiple detours are crammed into the same page:

- installing the 2nd one switches the page back to RW;
- the page holding the already-executing 1st detour becomes non-executable at the same time → crash.

Either give each slot its own page:

```cpp
__declspec(allocate(".uhook")) alignas(4096) inline unsigned char g_slot0[256];
__declspec(allocate(".uhook")) alignas(4096) inline unsigned char g_slot1[256];
__declspec(allocate(".uhook")) alignas(4096) inline unsigned char g_slot2[256];
__declspec(allocate(".uhook")) alignas(4096) inline unsigned char g_slot3[256];

inline unsigned char* const g_slots[4] = { g_slot0, g_slot1, g_slot2, g_slot3 };
```

That way, when you switch the permissions of slot 2, the page holding slot 1 is completely unaffected.

Or emit all detours at once and seal them all at the end: suitable for scenarios where every detour is installed in `OnLoad` and nothing changes afterwards. **Do not** go back and modify the same page after "a batch is installed and sealed".

> Putting `alignas(4096)` on a single array (for example `char g[4][256]`) does not work — the four segments are laid out contiguously and straddle pages, which amounts to sharing pages. You must split it into four separate variables so the linker page-aligns each of them.

---

#### 6.10.6 Do Not Put "base-dependent" References into the Generated Code

`rt.add` relocates the code to the JIT page's address, and then you copy the bytes somewhere else. Therefore:

- ✅ Use immediates for absolute addresses: `a.mov(reg, (uint64_t)ptr)`
- ✅ Relative jumps inside the code block (`a.jmp(label)`, `a.je(label)`): the whole block moves together, relative distances do not change, safe
- ❌ Referencing "the absolute address of the label itself", or using an address table: these are computed against the JIT address during relocation and become wrong after the move

When you need an absolute address, always cast to `(uint64_t)` and embed it as an immediate.

In addition, x86-64 **has no** `call imm64` encoding. To call an absolute address you must go through a register:

```cpp
ASMJIT_EMIT(a.mov(asmjit::x86::rax, (uint64_t)helper));
ASMJIT_EMIT(a.call(asmjit::x86::rax));
```

---

#### 6.10.7 When a detour Is Not Needed at All

If the need is only "change one constant inside an instruction" or "nop out a conditional jump", use `Patch` to change the bytes directly; no asmjit and no custom section are needed:

```cpp
static uint8_t kNewBytes[] = { 0x48, 0xC7, 0xC0, 0x00, 0x02, 0x00, 0x00 };
api->Patch(target, kNewBytes, sizeof(kNewBytes));
```

asmjit's only value in this scenario is sparing you from hand-writing that string of hex — just emit it, `memcpy` it into an ordinary buffer and hand it to `Patch`; **no `.uhook` section is needed, and the detour ownership check does not apply**.

The criterion is simple:

| What you need | Approach |
|--------|------|
| Insert logic before/after a function entry | Ordinary C++ detour (§6.2) |
| Change an instruction's immediate / opcode | `Patch` (§6.3); the bytes can be hand-written or generated with asmjit |
| Rewrite registers, jump back conditionally into the middle of a function | asmjit + custom section (this chapter) |

---

#### 6.10.8 Project Configuration

| Item | Value |
|----|----|
| Platform / configuration | `x64` / `Release` (consistent with the rest of the plugin) |
| Preprocessor macro | `ASMJIT_STATIC` (**required**; without it asmjit is treated as a DLL import and you get a pile of unresolved `__imp_` symbols) |
| Additional include directories | the directory containing the asmjit headers |
| Additional dependencies | `asmjit.lib` |
| Runtime library | must match how `asmjit.lib` was built. Mixing `/MT` and `/MD` reports LNK4098 and buries the hidden risk of cross-CRT allocation/free |

`ASMJIT_STATIC` has exactly one effect: it changes `ASMJIT_API` from `__declspec(dllimport)` to empty. If the definition is missing, all asmjit symbols become "imported from a DLL" and the link reports `error LNK2019: unresolved external symbol "__declspec(dllimport) ... asmjit::CodeHolder::CodeHolder"`.

asmjit's source itself is zlib licensed, so vendoring a copy into the plugin project is the least trouble (headers + build `asmjit.lib` once with CMake; make sure `ASMJIT_STATIC=ON` and `CMAKE_MSVC_RUNTIME_LIBRARY` match the plugin). No extra dependencies are needed — asmjit does not depend on any third-party library.

---

#### 6.10.9 asmjit Version Differences

API naming has changed between versions, so copying code across versions fails to compile:

| | 1.17 | 1.20 and later |
|---|---|---|
| Success error code | `kErrorOk` | `Error::kOk` |
| Code size | `code.codeSize()` | `code.code_size()` |
| Error string | `DebugUtils::errorAsString()` | `DebugUtils::error_as_string()` |
| Recommended header | `asmjit/x86.h` | `asmjit/x86.h` (the old `asmjit/asmjit.h` still works, but prints a deprecated notice) |
| ABI namespace | `asmjit::v1_17` | changes with the version (e.g. `asmjit::v1_21`) |

The version number is `ASMJIT_LIBRARY_VERSION` in `asmjit/core/api-config.h`. **The ABI namespace is the version gate at link time**: when the header and the .lib versions do not match, the symbols land in different namespaces and the link fails outright, instead of going wrong at runtime.

Different plugins, and a plugin and the Loader, each statically link their own asmjit and do not interfere with each other. But **within a single plugin** the same header and .lib must be used.

---

#### 6.10.10 Troubleshooting Table

| Symptom | Cause |
|------|------|
| `InstallHook rejected: caller/callback is not an active plugin` | the detour is not inside the plugin module (most likely the return value of `rt.add` was passed in directly) |
| Link reports unresolved `__imp_...asmjit...` | `ASMJIT_STATIC` was not defined |
| Link reports `LNK4098` default library conflict | the plugin runtime library (`/MT`, `/MD`) does not match `asmjit.lib` |
| Scrambled behavior after installation, no logs at all | an emit failure was silently dropped, no `ASMJIT_EMIT` check was done |
| The first detour works, crashes after installing the second | multiple detours share a page, sealing/rewriting interrupt each other |
| Access violation on the very first call | the page was still RW when executed; or `InstallHook` was called before switching to RX |
| Occasional crashes, does not reproduce after a restart | missing `FlushInstructionCache`, or the generated code embeds an absolute reference that changes with the base address |
| Hook fails to install on another machine / after a rebuild | the section was optimized away (the constant was unused), or `alignas` was omitted so slots straddle pages |

Self-check: after sealing you can confirm the page attributes

```cpp
MEMORY_BASIC_INFORMATION mbi{};
if (VirtualQuery(g_detourCode, &mbi, sizeof(mbi))) {
    // expected PAGE_EXECUTE_READ (0x20); if it is still PAGE_READWRITE(0x04), sealing failed
}
```

---

## 7. Adding Main Menu UI (FrontEnd)

The previous six chapters were all about **plugin DLLs**. This chapter is different: **to add a piece of UI of your own to the main menu**, you only write files — no DLL to compile, no `OnLoad`, and no need to bother with the `CIV6_PLUGIN_*` macros.

The only thing you have to do: write a pair of same-named `FrontEndUI_*.xml` + `.lua` and **just drop them into the mod directory** (no need to register them in `.modinfo`). The Loader does the rest — it looks them up by file name in your mod directory tree, and once the main menu is built it mounts the UI into it. You do not have to write a single line of C++.

> Applicable scenario: panels, buttons and popups on the **main menu** (FrontEnd). In-game UI (InGame) does not take this path, it still has to be declared with the native
> `AddUserInterfaces` + `ReplaceUIScript`.

### 7.1 Files and Naming

```text
MyAwesomeMod/
  MyAwesomeMod.modinfo          ← used only for the Mod's own enablement/metadata; the Loader does not read it
  UI/FrontEnd/
    FrontEndUI_MyMod.xml        ← UI definition
    FrontEndUI_MyMod.lua        ← logic, same name and same directory as the xml
  Binaries/Win64/
    GameCore_XP2_Shell_FinalRelease.dll
    Civ6DllLoader.runtime.dll
```

File names cannot be chosen freely: **if any one of the following is not met, this UI will not be loaded** (the log only contains a "found N" count, it does not point out which one was skipped):

| Requirement | Description |
|---|---|
| The prefix must be `FrontEndUI_` | This is the Loader's identifying marker; case-insensitive |
| The extension must be `.xml` | Only xml is searched; a same-named `.lua` is loaded together with it automatically |
| There must be at least one more character after the prefix | `FrontEndUI_.xml` is invalid |
| It must be placed inside a mod directory | The Loader only scans the directory trees of enabled mods, recursively up to 8 levels |

`.xml` and `.lua` must be **same-named and in the same directory**. Any subdirectory inside the mod will do — the Loader takes the **path relative to the mod root** (separators normalized to `/`, the `.xml` stripped) and hands it to the game for loading, so both `UI/FrontEnd/FrontEndUI_MyMod.xml` and `FrontEndUI_MyMod.xml` (placed directly at the mod root) work. If two mods contain a file with the same relative path, only one copy is loaded.

### 7.2 No Need to Declare It in .modinfo

The Loader **does not read** `<FrontEndActions><ImportFiles>`. Just put `FrontEndUI_*.xml` into the mod directory; an `ImportFiles` entry written in `.modinfo` has no effect whatsoever on the Loader.

> `docs/template/YourDllMod.modinfo` in this repository **does contain** a `<ImportFiles id="FrontEndUI">` block.
> That block is kept for the case of **not using the Loader and going through the game's native ForgeUI loading**; when the Loader is used it neither takes effect nor gets in the way.
> In other words: **it does not matter whether you add it or not, the Loader always scans by file name.**

The only side effect of doing it this way: **every file named `FrontEndUI_*.xml` in the same mod directory will be loaded**. Do not give this prefix to any other xml.

### 7.3 Conditional Loading Is Not Supported

**UI injected through this path is always loaded, and no conditions can be added.** The Loader only looks at the file name; any `criteria` in `.modinfo` does not participate in this decision.

If the UI really needs to appear conditionally, there are two paths:

- Let the UI **always be injected**, and decide inside the `.lua` whether it should be shown (`ContextPtr:SetHide(true)`);
- Switch to the native in-game UI mechanism (`AddUserInterfaces` + `ReplaceUIScript`); that path supports `criteria`.

### 7.4 Mod Status Requirements

- The mod must be **enabled** (one that has been unticked in the game does not count): the Loader only scans those mod directories registered as enabled in `Mods.sqlite`;
- The mod directory itself must be readable; subdirectories without permission encountered during scanning are skipped, and they do not interrupt the loading of other mods.

### 7.5 Check the Log When Something Goes Wrong

After entering the main menu, open `Civ6DllLoader_Loader.log` and search for `UiInject:`. Normally you should see two lines:

```
UiInject: found 2 Mod UI
UiInject: context loaded (FrontEndUI_MyMod)
```

Use them to locate the problem:

| Log situation | Where the problem is |
|---|---|
| No `found N Mod UI` | The Mod was not judged as enabled by `Mods.sqlite` (§7.4), or the scanning stage failed as a whole (the reason is in the log) |
| Not a single one of your own diagnostic logs | Did you wrap it in `DebugLogOn()`? That is gated on `ENABLE_DEBUG_LOG` and is off by default (§6.9) |
| There is `found N`, but not your own file name | The file name prefix/extension does not comply, or it was placed in a mod directory that is not enabled — compare with the table in §7.1 |
| There is `context loaded (your file name)`, but the UI is not shown | The problem is in the UI itself (the XML layout or the Lua did not show it), it has nothing to do with the Loader |

### 7.6 Notes

- **Main menu only**. In-game UI is not covered here (see the beginning of §7).
- **Unconditional loading** (see 7.3); `.modinfo` does not need an `ImportFiles` entry (see 7.2).
- **The UI is rebuilt whenever the main menu is rebuilt**. In extremely rare cases it is not re-injected this time, and the UI only comes back on the next main menu rebuild; restarting the game always restores it.

---

## 8. Troubleshooting (FAQ)

### Q: Does the player have to subscribe to or download the Loader separately as a prerequisite?
**A: No.** Every mod that adopts this system ships a shell and `Civ6DllLoader.runtime.dll` in `Binaries/Win64/` as specified. If the player installs the mod for the first time, the shell automatically completes the `version.dll` takeover bootstrap; if `version.dll` already exists, it always goes into steady-state operation.

### Q: Several different mods all ship `Civ6DllLoader.runtime.dll`, will that cause a version conflict?
**A: No.** The Loader's election mechanism automatically compares the runtimes of all enabled mods and **always picks the highest-version runtime.dll to load**. A newly released mod can therefore seamlessly carry older mods along to enjoy the newer Loader features and fixes.

### Q: Why does my plugin placed under `Binaries/Win64/Plugins/` not get loaded?
**A: Check the two gates first.**
1. **Directory**: the Loader strictly scans only `Plugins/Global/*.dll` and `Plugins/GameCore/*.dll`. DLLs lying directly in the `Plugins/` root are not recognized.
2. **Version resource**: the DLL must carry a `VERSIONINFO` file version (see §2.2). If it does not, the log will contain
   `Rejected plugin (no VERSIONINFO file version resource)`.

Also confirm the mod itself is enabled — the Loader only looks for plugins among the enabled mods recorded in `Mods.sqlite`.

### Q: My plugin clearly exists, but the log says it was displaced by another path?
**A: Same-name plugins are elected by version, only one copy is kept.** When multiple mods ship a DLL with the same name, the Loader loads only the one with the **highest file version**, the others are eliminated.
The log looks like:

```
Duplicate plugin basename: kept <winning path> (v1.2.0.0), dropped <your path> (v1.0.0.0, lower version)
```

When the versions are equal the decision is made by lexicographic order of the paths (`same version -> lexicographic tie-break`).
Raise the version number, or switch to a file name that will not collide. See §2.2 for details.

### Q: How do I let "several mods derived from the same base plugin" coexist?
**A: Just put different version numbers on the same file name.** The Loader picks the highest one to load, the rest step aside automatically.
This is also the recommended practice — only one image is running, so you never get two mods each installing the same hook and interfering with each other.

### Q: How do I solve the compile error `#error "CIV6: must declare plugin category"`?
**A: You must define the category macro in the project preprocessor.** Add `CIV6_PLUGIN_GLOBAL` (global plugin) or `CIV6_PLUGIN_GAMECORE` (gameplay plugin) under project properties "C/C++ -> Preprocessor -> Preprocessor Definitions".

### Q: Why does calling `RegisterGameCoreScriptForGamePlay` or `GetPlayer` fail to compile with a message saying it is not a member of HookAPI?
**A: You triggered the compile-time gate protection.** Your plugin defines `CIV6_PLUGIN_GLOBAL`, but these interfaces are GameCore gameplay-exclusive interfaces (their lifetime is bounded by match creation/destruction; calling them from a resident plugin causes null-pointer or illegal access). If it really is a gameplay-logic plugin, it should be turned into a GameCore plugin (define `CIV6_PLUGIN_GAMECORE` and put it in the `Plugins/GameCore/` directory).

### Q: Do plugins need to be recompiled and released after a game update?
**A: Usually not.** The Loader has built-in cross-version correction (`PdataMatch`); if the plugin uses the `HookInfo`, `ResolveByHookInfo` or `ResolveByName` style recommended in Section 5, the old RVA or symbol name is automatically corrected to the new address after an update. If resolution fails, check the warnings in `Documents/My Games/Sid Meier's Civilization VI/Logs/Civ6DllLoader_Loader.log`.

### Q: Is there any cleanup to do in `OnUnload`?
**A: No, keep it an empty implementation or simply do not export it.** The Loader reclaims hooks, parameter captures and message subscriptions automatically; when leaving a match or reloading, the whole GameCore module is fully released by the engine. See the lifecycle description in §3.

### Q: I changed memory with `Patch`; will it be restored automatically when the plugin is unloaded?
**A: No.** The Loader does not roll patches back automatically; the bytes written stay in effect until the process exits. Patches should therefore be designed as "take effect and never restore". `Unpatch` can be used to revoke something dynamically at runtime, but do not treat it as a routine unload-cleanup step (see §6.3).

### Q: I only want to add a panel to the main menu, do I still have to write a C++ plugin?
**A: You do not.** Just put a pair of `FrontEndUI_*.xml` + `.lua` into the mod directory; you do **not** need to compile a DLL, and you do **not** need to write `<ImportFiles>` in `.modinfo` — the Loader scans by file name (see §7.2). See §7 for details.

### Q: The main menu UI does not appear, how do I troubleshoot it?
**A:** Check in order — ① whether the file name starts with `FrontEndUI_` and ends with `.xml`, and is same-named and in the same directory as the `.lua`; ② whether the file is inside the directory tree of an **enabled** mod (recursion limit 8 levels); ③ whether `Civ6DllLoader_Loader.log` contains `UiInject: found N Mod UI` and `UiInject: context loaded (your file name)`.

Note: whether or not you write `<ImportFiles>` in `.modinfo` **has no effect** on the Loader (§7.2) — do not register it by following the official UI tutorials, it is a waste of time. See §7.5 for details.

---

## Appendix A: Complete HookAPI Interface Table

`HookAPI` is the function pointer table on the plugin side, filled in by `HookAPI_Resolve(ctx)` in `OnLoad` by name; every cell is either a valid function address or `nullptr`.
All 80 entries are listed below, grouped by purpose.

**General conventions**:
- Every `void* L` parameter is a Lua state pointer and **must** be the value returned by `GetMainLuaState()`, or the `L` passed in by a callback (`ScriptGlobalsFn` / `GameEventPushFn` / `LuaMethodFunc`); passing `nullptr` is rejected and logged.
- All Lua stack indices follow the standard Lua convention: positive numbers count from the bottom of the stack, negative numbers count from the top.
- Interfaces marked ✱ are **GameCore-exclusive**; calling them from a Global plugin is an error at **compile time** (see §4).

### A.1 Image Queries and Address Resolution

| Interface | Signature | Description |
|------|------|------|
| `IsImageLoaded` | `bool(HookImageId)` | Whether that image is currently loaded. Used to tell whether GameCore is ready. |
| `GetImageInfo` | `bool(HookImageId, HookImageInfo*)` | Get the image base address and size. Set `outInfo->structSize` to `sizeof(HookImageInfo)` before calling. |
| `ResolveByRva` | `void*(HookImageId, uintptr_t rva, size_t span)` | Resolve by the **old-version RVA**, automatically corrected to the current version through `PdataMatch`. `span` is the number of bytes required to be readable, default 1. When the symbol DB has no such image, no correction is done and it is resolved directly by the original value (see §5.7). |
| `ResolveByName` | `void*(HookImageId, const char*)` | Resolve by symbol name (both demangled friendly names and MSVC decorated names are supported), also going through cross-version correction. |
| `ResolveByRvaEx` | `void*(HookImageId, uintptr_t rva, size_t span, HookImageSection)` | Raw RVA **without** correction; the section can be specified. Use only when you have already worked out the current-version coordinate yourself. |
| `ResolveByPattern` | `void*(HookImageId, const char* pattern, HookImageSection, uint32_t index)` | AOB signature search (syntax in §5.1), returns the `index`-th ascending hit within the specified section (1-based; passing 0 is treated as 1). |
| `ResolveByVTable` | `void*(HookImageId, const char* vtableSymbol, uint32_t index)` | Resolve a virtual function address by vtable symbol name + slot number (0-based). |
| `GetSymbolDbState` | `HookApiSymbolDbState(HookImageId)` | A snapshot of the match state between the symbol DB and the image. Optional, used to distinguish "the function does not exist" from "the symbol DB is unavailable", see §5.7. |
| `GetSymbolDbMatchRatio` | `double(HookImageId)` | The match ratio of cross-version correction `[0.0, 1.0]`, suitable for writing into logs for troubleshooting, see §5.7. |

```cpp
// The typical "with fallback" three-step style:
void* fn = ResolveByName<void*>(api, kHookImageGameCoreXP2, "GameCore::Foo::Bar");
if (!fn) fn = ResolveByPattern<void*>(api, kHookImageGameCoreXP2, "48 89 5C 24 ?? 57", kSectionText);
if (!fn) { api->Log("Foo::Bar not found; feature disabled"); return; }
```

> The more recommended style is to use `HookInfo` + `ResolveByHookInfo`, which builds the fallback logic above in (see §5.2).

### A.2 Hooks, Patches and Variable Capture

| Interface | Signature | Description |
|------|------|------|
| `InstallHook` | `bool(void* target, void* detour, void** original)` | Install a MinHook detour; the original function's trampoline is written into `*original`. The Loader removes the hook automatically when the plugin is unloaded. |
| `Patch` | `bool(void* target, const void* bytes, size_t len)` | Raw byte write, used to modify the instructions themselves. The same `target` can only be patched once per plugin; sites that are already hooked are rejected. See §6.3. |
| `Unpatch` | `bool(void* target)` | Restore your own layer of patching. **Use with caution**: it can only remove the top layer of the stack, do not use it as unload cleanup, see §6.3. |
| `RegisterGlobalCapture` | `bool(void* providerAddress, uint32_t paramIndex, void** out)` | Intercept the `paramIndex`-th **integer** register parameter of `providerAddress` (0–3 correspond to rcx/rdx/r8/r9; floating-point parameters go through xmm and cannot be captured) and write it into `*out`. Multiple plugins can stack subscriptions on the same address. This is the standard means of obtaining a non-exported global object, see §5.6. |
| `UnregisterGlobalCapture` | `bool(void* providerAddress, void** out)` | Revoke one capture subscription this plugin previously made on that provider (all three match). It only removes the subscription, not the hook; after the last subscription is removed it returns to the fast path. See §5.6. |

```cpp
// Common template for InstallHook (note that the detour must use the same calling convention as the target function):
using BarFn = int(__fastcall*)(void* self, int a);
static BarFn g_origBar = nullptr;

int __fastcall DetourBar(void* self, int a) {
    if (a < 0) a = 0;                 // upfront correction
    return g_origBar(self, a);        // call the original function
}

if (void* t = ResolveByName<void*>(api, kHookImageGameCoreXP2, "GameCore::Foo::Bar")) {
    InstallHook(api, t, reinterpret_cast<void*>(&DetourBar), &g_origBar);
}
```

```cpp
// Capturing a singleton that cannot be obtained through a symbol:
static void* g_pWorld = nullptr;
if (void* provider = ResolveByName<void*>(api, kHookImageGameCoreXP2, "GameCore::World::Init")) {
    api->RegisterGlobalCapture(provider, 0, &g_pWorld);   // take the 1st parameter (rcx)
}
// Afterwards g_pWorld is filled in automatically; just check it for null before use.
```

### A.3 Message Bus (Communication Between Plugins)

| Interface | Signature | Description |
|------|------|------|
| `RegisterMessageHandler` | `bool(MessageHandlerFn)` | Register a message callback. The **address is fixed to the plugin DLL file name** (extension stripped, UTF-8 case-sensitive); custom names are not accepted. |
| `BroadcastMessage` | `void(const char* topic, const void* data, size_t len)` | Broadcast to all registered plugins. |
| `SendMessageTo` | `void(const char* pluginName, const char* topic, const void* data, size_t len)` | Point-to-point send; `pluginName` is the other party's DLL base file name. |

If the plugin exports `OnMessage`, the Loader registers it automatically with the plugin file name after `OnLoad` returns, so **you usually do not need to write** `RegisterMessageHandler` by hand.

```cpp
extern "C" __declspec(dllexport)
void OnMessage(const char* topic, const void* data, size_t len) {
    if (strcmp(topic, "config-changed") == 0) { /* ... */ }
}

// Sending a message proactively:
api->BroadcastMessage("config-changed", nullptr, 0);
api->SendMessageTo("OtherPlugin", "ping", "hi", 2);
```

### A.4 Lua State and Stack Operations

The following interfaces map directly onto the exports of HavokScript, the Lua virtual machine embedded in the game engine; their semantics are the same as the standard Lua 5.1 C API. When any underlying import is missing, the interface logs and returns a safe default (0 / nullptr), **it will not crash**.

| Category | Interfaces |
|------|------|
| Getting the state | `GetMainLuaState` (callable from both Global and GameCore, game thread only, survives a match change) |
| Stack operations | `LuaGetTop` `LuaPop` `LuaPushValue` `LuaPushNil` `LuaToCFunction` |
| Pushing | `LuaPushInteger` `LuaPushNumber` `LuaPushBoolean` `LuaPushLightUserData` `LuaPushFString` |
| Reading | `LuaToInteger` `LuaToNumber` `LuaToBoolean` `LuaToUserData` `LuaType` `LuaIsNumber` `LuaIsUserData` `LuaObjLen` |
| Argument checking | `LuaCheckInteger` `LuaCheckNumber` `LuaCheckLString` `LuaCheckTable` `LuaOptInteger` |
| Table operations | `LuaCreateTable` `LuaGetField` `LuaSetField` `LuaGetTable` `LuaSetTable` `LuaRawGetI` `LuaNext` |
| References | `LuaRef` `LuaUnref` |

**Use `LuaNext` to iterate a hash table**. `LuaObjLen` only gives the length of the sequence part, it cannot get string keys. `LuaNext(L, index)`
is a direct wrapper of Lua 5.1 `lua_next`, and its stack contract is copied as well:

```cpp
// Assume the table to iterate is at tblIndex (a negative index changes as things are pushed, so convert it to an absolute index first)
const int t = tblIndex < 0 ? api->LuaGetTop(L) + tblIndex + 1 : tblIndex;
api->LuaPushNil(L);                        // first round: push nil in the key position
while (api->LuaNext(L, t)) {               // returns 1: the stack gained key(-2) / value(-1)
    // LuaType returns the standard Lua 5.1 type codes: string = 4, number = 3 (HookAPI does not define extra constants)
    const char* k = api->LuaType(L, -2) == 4 ? api->LuaCheckLString(L, -2, nullptr) : nullptr;
    const double  v = api->LuaToNumber(L, -1);
    // ... handle this entry ...
    api->LuaPop(L, 1);                     // key point: pop the value yourself, leaving only the key for the next round
}
```

Two things that are easy to trip over:

- **The value must be popped by yourself**. `LuaNext` is only responsible for popping the key and pushing the next key/value pair; not popping the value makes the next round
  start from a wrong position, and the iteration will skip entries or even loop forever;
- **Do not modify the table during iteration**. Lua 5.1 leaves the behaviour of "adding/removing keys while iterating" undefined; if you need to change it, collect into your own container first,
  and write back after the iteration ends.

Grouped by purpose:

| Purpose | Entries |
|---|---|
| Calling | `LuaPcall` `LuaCPcall` `LuaDoString` |
| Debugging | `LuaOpenDebug` `LuaSetHook` `LuaGetHook` `LuaGetHookMask` `LuaGetHookCount` |
| Errors | `LuaError` (throws a Lua error, **does not return**) |
| Civ6-specific | `LuaCheckPlayerId` (verifies that the stack top is a valid player ID, returns -1 if invalid) |

```cpp
// LuaDoString: execute a piece of Lua code directly
void* L = api->GetMainLuaState();
if (L) {
    if (api->LuaDoString(L, "print('hello from plugin')") != 0) {
        api->Log("LuaDoString failed");
    }
}
```

```cpp
// LuaCPcall: a protected C call, used to safely call the plugin's own C function
static int __fastcall MyCallback(void* L) {
    api->LuaPushInteger(L, 123);
    return 1;
}
const int rc = api->LuaCPcall(L, &MyCallback, nullptr);
// rc == 0 success; rc > 0 = engine Lua error state; rc < 0 = invalid argument / missing import
```

### A.5 Lua Global Tables and Method Extension

| Interface | Signature | Description |
|------|------|------|
| `LuaNewGlobalTable` | `void(void* L, const char* name, int narr, int nrec)` | Create a global table and leave it on the stack top. `narr`/`nrec` are array/hash preallocation hints. |
| `LuaPushCFunctionField` | `void(void* L, int tableIndex, const char* field, void* fn, const char* internalName)` | Attach a C function field to a table. `internalName` is the name used by the engine side for debugging. |
| `LuaPushNamedCClosure` | `void(void* L, void* fn, int nup, const char* name, int narr)` | Push a named C closure. |
| `RegisterGameCoreScriptForGamePlay` ✱ | `void(ScriptGlobalsFn)` | Register a callback for when the GameCore gameplay Lua build is complete. |
| `RegisterGameCoreScriptForUI` ✱ | `void(ScriptGlobalsFn)` | Same as above, but for GameCore's UI script Lua. |
| `RegisterGlobalScript` | `void(ScriptGlobalsFn)` | Register onto the exe's engine main Lua state (process-wide, survives a match change). Both Global and GameCore plugins can use it, see §6.6. |
| `PushLuaMethods` ✱ | `HookApiPushMethodsResult(const char* pushMethodsSymbol, int sigType, const LuaMethodInfo*, uint32_t count)` | Attach methods to an existing C++ Lua object, see §6.5. |

> The first three only manipulate the Lua stack and are **not restricted by plugin category**; the last four involve GameCore's Lua objects and environment, so they carry a category gate.

```cpp
// Global table injection (the full version of §6.6):
static int __fastcall lAdd(void* L) {
    const int a = api->LuaCheckInteger(L, 1);
    const int b = api->LuaCheckInteger(L, 2);
    api->LuaPushInteger(L, a + b);
    return 1;
}

static void InitMyMod(void* L) {
    api->LuaNewGlobalTable(L, "MyMod", 0, 4);
    api->LuaPushCFunctionField(L, -1, "Add", reinterpret_cast<void*>(&lAdd), "lAdd");
    api->LuaPop(L, 1);
}

api->RegisterGameCoreScriptForGamePlay(&InitMyMod);   // Lua: MyMod.Add(1, 2) -> 3
api->RegisterGameCoreScriptForUI(&InitMyMod);          // also available in GameCore UI Lua
```

```cpp
// Add methods to an existing engine Lua object (the first parameter is the full symbol name, it is not concatenated automatically):
api->PushLuaMethods("GameCore::Lua::IMyManager::PushMethods", kHookApiPushNoManager, s_methods, count);
// sigType: kHookApiPushNoManager (no manager prefix) / kHookApiPushWithManager (with manager signature)
```

> `LuaMethodInfo` must be **C layout** and all three fields must be `const char*` literals; do **not** pass `std::string` / `std::vector` across DLLs.

### A.6 Game Event Bus

| Interface | Signature | Description |
|------|------|------|
| `RegisterGameEvent` ✱ | `bool(const char* name, GameEventPushFn)` | Register a custom GameEvent. The name is at most 95 bytes; longer ones return false. Registering the same name again is idempotent and returns true. |
| `TriggerGameEvent` ✱ | `bool(const char* name, const void* payload)` | Trigger it once. It must be registered first; `payload` is passed synchronously to `pushFn` and is only valid during this call. |

```cpp
static int PushArgs(void* L, const void* payload) {
    const auto* p = static_cast<const MyStruct*>(payload);
    api->LuaPushInteger(L, p->a);      // argument 1
    api->LuaPushBoolean(L, p->flag);   // argument 2
    return 2;                          // return the number of arguments pushed
}

api->RegisterGameEvent("MyModEvent", &PushArgs);

MyStruct s{ 42, true };
api->TriggerGameEvent("MyModEvent", &s);
```
```lua
GameEvents.MyModEvent.Add(function(a, flag) print(a, flag) end)
```

> **It must be called on the game thread.** The Loader handles taking and releasing the engine lock internally; if it cannot determine the lock state, it would rather refuse to trigger than re-enter Lua while holding the lock.

### A.7 Localization

| Interface | Signature | Description |
|------|------|------|
| `LocCreate` | `void*()` | Create a localization string object. It must be `LocDestroy`ed when done. |
| `LocDestroy` | `void(void*)` | Destroy the object. |
| `LocLookup` | `void*(void* loc, const char* key)` | Look up by key (such as `"LOC_TECH_POTTERY_NAME"`). |
| `LocPushArgument` | `int(void* loc, const char* arg)` | Append a formatting argument (corresponding to placeholders like `{1_TEXT}`). |
| `LocToUtf8` | `const char*(void*)` | Get the resulting UTF-8 string. |
| `LocStringBytes` | `uint32_t()` | Returns the number of bytes needed for the stack buffer (currently 256), for use with `CIV6_HOOK_API_LOC_STRING_BYTES`. |
| `LocalizationLookup` | `void*(void* outStr, const char* key)` | Allocation-free lookup: `outStr` points at the caller's own stack buffer. |

```cpp
// Approach one: allocation-free (better performance, recommended on hot paths)
char buf[CIV6_HOOK_API_LOC_STRING_BYTES];
if (api->LocalizationLookup(buf, "LOC_TECH_POTTERY_NAME")) {
    api->Log("Tech: %s", buf);
}

// Approach two: formatting with arguments
void* loc = api->LocCreate();
api->LocLookup(loc, "LOC_UNIT_GAINED_EXPERIENCE");
api->LocPushArgument(loc, "Warrior");
const char* text = api->LocToUtf8(loc);
api->Log("Msg: %s", text ? text : "");
api->LocDestroy(loc);
```

### A.8 Game State and Utilities

| Interface | Signature | Description |
|------|------|------|
| `GetPlayer` ✱ | `void*(int playerId)` | Get the player object pointer. Returns nullptr before the match starts. |
| `MakeHash` | `uint32_t(const char*)` | CRC32 that is bit-for-bit identical to the engine's `GameCore::Utilities::MakeHash`. **Purely local computation**, usable before a match and after a reload. |
| `Log` | `void(const char* fmt, ...)` | Write to `Civ6DllLoader_Loader.log`. |

```cpp
uint32_t h = api->MakeHash("QUEST_SEND_TRADE_ROUTE");   // identical to the engine's internal hash
api->Log("hash of quest = 0x%08X", h);
```

> **About the GameCore lock**: for high-level interfaces such as `TriggerGameEvent` and `PushLuaMethods` that need to enter Lua,
> taking and releasing the lock is done by the Loader internally, the plugin side has **no such interface and does not need one**, and in the vast majority of scenarios you do not have to take the lock manually.
> If a plugin calls engine internal functions directly, it is responsible for the lock itself, and **must do so under SEH protection** — if an exception is thrown when reacquiring after releasing,
> the lock will never be taken back and the game will simply hang.

### A.9 Initialization and Compatibility Checks

```cpp
static inline int      HookContext_IsCompatible(const HookContext* ctx);
inline        HookAPI* HookAPI_Resolve(const HookContext* ctx);

template <typename... M>
inline bool HookAPI_RequireAll(const HookAPI* api, M... members);
```

| Helper in the header | Description |
|--------------------|------|
| `HookContext_IsCompatible(ctx)` | Check whether the Loader and the plugin are from the same build (compares `structSize` + `apiVersion`). Call it at the start of `OnLoad`; if it returns 0, `return 0` to make loading fail. |
| `HookAPI_Resolve(ctx)` | Resolve the 80 entries by name into a table private to this module, and return its pointer. Idempotent; multiple `.cpp` files of the same plugin share the same one. |
| `HookAPI_RequireAll(api, &HookAPI::A, &HookAPI::B)` | Check whether the specified entries all resolved successfully. List only the ones you use; a missing item returns false, for a graceful exit. |
| `ResolveByName<T>(api, image, name)` | A return-type cast wrapper around `ResolveByName`. |
| `ResolveByRva<T>(api, image, rva, span)` | Same as above. |
| `ResolveByRvaEx<T>(api, image, rva, section, span)` | Same as above. |
| `ResolveByPattern<T>(api, image, pattern, section, index)` | Same as above, `index` defaults to 1. |
| `ResolveByVTable<T>(api, image, sym, index)` | Same as above. |
| `InstallHook(api, target, detour, &orig)` | Saves you the manual `reinterpret_cast`. |
| `HookRva` / `HookName` / `HookVTable` | `constexpr` factories that build a `HookInfo`. |
| `ResolveByHookInfo(api, image, info, span)` | Resolve a `HookInfo` (automatically falls back to AOB when the primary path fails). |
| `ResolveByHookInfoExe(api, infoExe, span)` | Detect DX11/DX12 automatically, then resolve. |
| `ResolveDataByAob(api, image, aob, index)` | RIP-relative addressing, resolves out the global data slot. |

---

## Appendix B: Complete Minimal Working Plugin Example

A GameCore plugin: injects `MyMod.Version()` into Lua, and prints one log line when entering a match.

**Project settings**: x64 / Release, preprocessor definition `CIV6_PLUGIN_GAMECORE`, include `HookAPI.h`.

```cpp
// MyGameplayPlugin.cpp
#include "HookAPI.h"
#include <string.h>

static HookAPI* g_api = nullptr;

static int __fastcall lVersion(void* L) {
    g_api->LuaPushFString(L, "MyMod v1.0");
    return 1;
}

static void OnInitGlobals(void* L) {
    g_api->LuaNewGlobalTable(L, "MyMod", 0, 2);
    g_api->LuaPushCFunctionField(L, -1, "Version", reinterpret_cast<void*>(&lVersion), "lVersion");
    g_api->LuaPop(L, 1);
}

extern "C" __declspec(dllexport) int OnLoad(HookContext* ctx) {
    if (!HookContext_IsCompatible(ctx)) {
        return 0;
    }
    HookAPI* api = HookAPI_Resolve(ctx);
    // You must list every entry that is [actually used] — including those called indirectly from callbacks.
    // OnInitGlobals / lVersion below use Lua tables and string pushing,
    // omitting them means calling through a null pointer (this was stated among the key points of §3 of this document, and here is where it is easiest to get wrong).
    if (!HookAPI_RequireAll(api,
            &HookAPI::Log,
            &HookAPI::RegisterGameCoreScriptForGamePlay,
            &HookAPI::LuaNewGlobalTable,
            &HookAPI::LuaPushCFunctionField,
            &HookAPI::LuaPushFString,
            &HookAPI::LuaPop)) {
        return 0;
    }
    g_api = api;
    api->Log("MyMod: loaded");

    api->RegisterGameCoreScriptForGamePlay(&OnInitGlobals);
    return 1;
}

// Optional: receive messages from other plugins
extern "C" __declspec(dllexport)
void OnMessage(const char* topic, const void* data, size_t len) {
    g_api->Log("MyMod: got topic=%s len=%zu", topic ? topic : "?", len);
}
```

**Deployment**: put the compiled `MyGameplayPlugin.dll` into `MyMod/Binaries/Win64/Plugins/GameCore/`.

**Verification**: after entering a match, open FireTuner and execute `print(MyMod.Version())`; it should output `MyMod v1.0`; at the same time `MyMod: loaded` should appear in `Civ6DllLoader_Loader.log`.
