# Civ6BasePlugin_GameCore

> English | [中文](README.md)

The **GameCore lifecycle plugin project** for Civilization VI, running on the **HookAPI** plugin architecture of the [Civ6DllLoader] runtime.

- **Plugin category**: GameCore (macro `CIV6_PLUGIN_GAMECORE`), entry point `OnLoad(HookContext* ctx)`
- **Lifecycle**: loaded on every **match entry**, and unloaded together with the whole GameCore image when **leaving the match / returning to the main menu / reloading the match**
- **Deployment location**: `<Mod>/Binaries/Win64/Plugins/GameCore/Civ6BasePlugin_GameCore.dll`
- **Build**: MSBuild (Release | x64, C++20, `/utf-8`, `/MT`)

Sister project: [Global side](../../Global/Civ6BasePlugin/README.md) (resident process, including ModConfig and render limit unlock).

---

## Module list

Every module has its own README explaining which defect it fixes, how it is located, and which pitfalls were hit.

### BugFix — Fixing vanilla defects

| Module | What problem it solves |
| --- | --- |
| [GreatWorkOptimizer](dll/BugFix/GreatWorkOptimizer/README.md) | Stutter when the late-game AI computes Great Work assignment (O(G³) recomputation) |
| [GreatWorkTypeLookup](dll/BugFix/GreatWorkTypeLookup/README.md) | Great Work types / Great Work slot types added by mods cannot be recognized by the engine |
| [GreatWorkYieldStack](dll/BugFix/GreatWorkYieldStack/README.md) | Yield bonuses from multiple sources for the same Great Work type do not stack, only one of them takes effect |
| [GreatWorkPreserve](dll/BugFix/GreatWorkPreserve/README.md) | When the number of extra Great Work slots changes, all Great Works already placed in that building are lost |
| [GreatWorkModifierAttach](dll/BugFix/GreatWorkModifierAttach/README.md) | Modifiers attached to a Great Work are not triggered after the Great Work is created (both the Great Person activation path and the Lua interface path) |
| [InitUnitReturn](dll/BugFix/InitUnitReturn/README.md) | `InitUnitValidAdjacentHex` returns nil, so the created unit cannot be obtained |
| [UnitDamageCap](dll/BugFix/UnitDamageCap/README.md) | Once unit HP exceeds 100, damage cannot kill |

### APIExtension — Adding Lua interfaces

| Module | Which interface it adds |
| --- | --- |
| [GreatWorkAPI](dll/APIExtension/GreatWorkAPI/README.md) | `GreatWorkManager.AddGreatWork` / `AddGreatWorkToCity` / `RemoveGreatWork` (supports multiple copies of the same Great Work) |
| [ModifierManagerAPI](dll/APIExtension/ModifierManagerAPI/README.md) | `unit:AttachModifierByID` / `ModifierManager.DetachModifierByID` / `GetCount` |
| [UnitLevelAPI](dll/APIExtension/UnitLevelAPI/README.md) | `unit:GetExperience():SetLevel(n)` |
| [GovernorAPI](dll/APIExtension/GovernorAPI/README.md) | `Player:GetGovernors():SetAvailableGovPoints(n)` |
| [PlotAPI](dll/APIExtension/PlotAPI/README.md) | `Plot:ChangeAppeal(n)` (accumulating delta, real data untouched) |

### GameplayExtension — Adding game objects the engine does not originally have

| Module | What it registers |
| --- | --- |
| [CustomRequirement](dll/GameplayExtension/CustomRequirement/README.md) | Two custom **Requirement**s: `REQUIREMENT_PLAYER_PROPERTY_MATCHES` (player dynamic property threshold), `REQUIREMENT_PLOT_ROUTE_TYPE_MATCHES` (plot route type). The former installs 4 hooks, the latter installs 3 |
| [AdjustPlotPropertyEffect](dll/GameplayExtension/AdjustPlotPropertyEffect/README.md) | One custom **Effect**: `EFFECT_ADJUST_PLOT_PROPERTY` (changes plot dynamic properties). It does not change hooks; it clones the factory and effect vtable of `AdjustUnitProperty` |

> These two categories are **two sides of the same thing**: `Requirement` decides "whether the condition holds", `Effect` decides "what to do once it holds".
> Both of them have to inject custom objects into the `GameEffects` registry, so they are grouped under the same `GameplayExtension`.
> The accompanying SQL example is in `Civ6Mod/Data/Gameplay.sql`.

---

## Compatible address resolution

All targets uniformly go through the `HookName(symbol name)` main path plus a unique AOB fallback (see `dll/Common/GameCoreResolve.h`).
When a bare RVA does not match between the symbol DB and the current image, it is **passed through as-is** by the Loader, which amounts to installing the hook on the wrong function.

> **No function prologue check.** A function entry may be rewritten by another plugin's hook into
> `E9 xx xx xx xx`; in that case "comparing the original entry bytes" necessarily fails, and the
> failure reason cannot be distinguished from "the game updated".
> For the relevant conditions and exceptions see [Plugin development guide §5.9](../../../docs/PLUGIN_GUIDE.md).

Symbol names always use the **demangled** form (looked up in reverse with `tools/rva_to_symbol.exe`), which reads best:

```bash
tools\rva_to_symbol.exe -gamecore 0x3A5A60 --demangled
```

> The input is an **RVA, not a VA**: absolute addresses in IDA must first have the image base subtracted.

**Exception**: targets that have overloads or template instantiations keep **mangled** names. For those, the symbol DB only gives pure ordinals such as `_1` / `_2` / `_26`
(`MakeObject_10` gives no hint at all that it is the one taking `Unit`), whereas the decorated name is instead more readable and also more stable.
The "Targets" table in each module README annotates entry by entry whether an AOB is carried and which symbol form is used.

### Uniqueness of the AOB fallback

Every AOB **hits exactly once** inside the `.text` section of the current `GameCore_XP2_FinalRelease.dll`
(verified by scanning the PE entry by entry). **Targets with too many look-alike functions carry no AOB** — carrying one would only randomly hit a different function.

---

## Directory structure

```
recipes/GameCore/Civ6BasePlugin_GameCore/
├── README.md
└── dll/
    ├── Civ6BasePlugin_GameCore.vcxproj
    ├── Civ6BasePlugin_GameCore.vcxproj.filters
    ├── HookAPI.h
    ├── framework.h / pch.h / pch.cpp
    ├── dllmain.cpp                          # OnLoad / OnUnload
    ├── include/asmjit/                      # vendored asmjit (used only by UnitDamageCap)
    ├── lib/asmjit.lib
    ├── Common/GameCoreResolve.h             # compatible address resolution (symbol name main path + AOB fallback)
    ├── BugFix/
    │   ├── GreatWorkOptimizer/    + README.md
    │   ├── GreatWorkTypeLookup/   + README.md
    │   ├── GreatWorkYieldStack/   + README.md
    │   ├── GreatWorkPreserve/     + README.md
    │   ├── GreatWorkModifierAttach/ + README.md
    │   ├── InitUnitReturn/        + README.md
    │   └── UnitDamageCap/         + README.md
    └── APIExtension/
        ├── GreatWorkAPI/          + README.md
        ├── ModifierManagerAPI/    + README.md
        ├── UnitLevelAPI/          + README.md
        ├── GovernorAPI/           + README.md
        └── PlotAPI/               + README.md
```

---

## Build

```powershell
& 'E:\VisualStudio\Products\MSBuild\Current\Bin\amd64\MSBuild.exe' `
  'recipes\GameCore\Civ6BasePlugin_GameCore\dll\Civ6BasePlugin_GameCore.vcxproj' `
  /p:Configuration=Release /p:Platform=x64
```

Output: `x64/Release/Civ6BasePlugin_GameCore.dll`.

Project configuration: `CIV6_PLUGIN_GAMECORE`, `ASMJIT_STATIC`, C++20, `/utf-8`,
**`/MT` (`MultiThreaded`)** — the latter is a hard requirement: the vendored `asmjit.lib` is
compiled with `MT_StaticRelease`, and using the default `/MD` produces `LNK2038` runtime library mismatch directly.

---

## Deployment and verification

1. Put `Civ6BasePlugin_GameCore.dll` into `<Mod>/Binaries/Win64/Plugins/GameCore/`;
2. Start the game and **enter a match** (GameCore plugins are loaded only when entering a match);
3. Look at `Civ6DllLoader_Loader.log`; normally the following should appear:

```
Civ6BasePlugin_GameCore: OnLoad
[GreatWorkOptimizer] installed 2/2 hooks; buildingDefs=ok humanSkip=on
[GreatWorkTypeLookup] GreatWorkSlotType hooked; resolver=ok (modded types now resolvable)
[GreatWorkTypeLookup] GreatWorkObjectType hooked; resolver=ok (modded types now resolvable)
[GreatWorkYieldStack] hooked GetGreatWorkYieldChange; great work yields now stack
[GreatWorkPreserve] active: extra great work slot changes keep existing great works
[GreatWorkModifierAttach] active: great works from Great Person now fire GreatWorkAdded
[InitUnitReturn] active: InitUnitValidAdjacentHex now returns the unit instance
[UnitDamageCap] hooked SetDamage+0x27; Range upper bound now Unit::GetMaxDamage() (detour=NNN bytes, orig=0x...)
[GreatWorkAPI] symbols ok; GreatWorkManager.AddGreatWork/AddGreatWorkToCity/RemoveGreatWork registered
[ModifierManager] unit-method inject: reg=1 pushed=1 (unit:AttachModifierByID(id))
[UnitLevelAPI] experience-method inject: reg=1 pushed=1 (unit:GetExperience():SetLevel(n))
[GovernorAPI] active: SetAvailableGovPoints pushed=1
[PlotAPI] active: ChangeAppeal pushed=1
Civ6BasePlugin_GameCore: loaded
```

If any module reports `target not resolvable`, that target drifted after a game update:
**that module is skipped automatically and the other modules keep working**, with no crash.

4. Verify the Lua extensions in FireTuner:

```lua
-- Great Work
print(GreatWorkManager.AddGreatWork(0, 0))            -- playerID, greatWorkIndex
print(GreatWorkManager.AddGreatWorkToCity(0, 1, 2))   -- playerID, cityID, greatWorkIndex
print(GreatWorkManager.RemoveGreatWork(0, 0))         -- playerID, greatWorkType (removes only the first copy)

-- Modifier
print(ModifierManager ~= nil)
unit:AttachModifierByID("MODIFIER_ID")                -- returns the index, or -1

-- Governor points / Appeal
Players[0]:GetGovernors():SetAvailableGovPoints(3)    -- -> true (sets "available"; spent is unchanged)
Map.GetPlot(10, 10):ChangeAppeal(3)                   -- -> true (relative delta, accumulates)

-- Unit level
unit:GetExperience():SetLevel(3)                      -- -> true
print(unit:GetExperience():GetLevel())                -- -> 3
print(unit:GetExperience():GetExperienceForNextLevel())  -- the threshold changes with the level
```

> See each module's README for its specific verification method and log samples.

---

## Notes

- **Both `/utf-8` and `/MT` are required**: the former because the repository's `.editorconfig` convention is UTF-8 without BOM;
  the latter because the vendored `asmjit.lib` is compiled against the static runtime library;
- **Only x64 is supported**;
- **GameCore reload**: the Loader clears the hooks and subscriptions that fall inside that image when GameCore unloads; if a plugin
  has registered subscriptions on the exe / resident module domain, it needs to register them again on the next `OnLoad`;
- **`OnUnload` should stay lightweight**: hooks and script registrations are reclaimed uniformly by the Loader per owner; each module's
  `*_Shutdown()` only drops references — do not perform complex destruction or free memory that has already been patched to point elsewhere;
- **asmjit is used in this project only for `UnitDamageCap`** (the Global side has another set for `UnlockModelLimit`);
  **Zydis is not needed** (that is for disassembly verification; this project only generates code).

---

## References

- Repository overview: [`README.md`](../../../README.md)
- Plugin development specification: [`docs/PLUGIN_GUIDE.md`](../../../docs/PLUGIN_GUIDE.md)
- Global side sister project: [`recipes/Global/Civ6BasePlugin/README.md`](../../Global/Civ6BasePlugin/README.md)

[Civ6DllLoader]: https://github.com/sociometry/Civ6DllLoader
