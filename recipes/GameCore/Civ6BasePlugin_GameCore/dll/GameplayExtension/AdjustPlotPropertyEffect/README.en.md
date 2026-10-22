# AdjustPlotPropertyEffect

> English | [中文](README.md)

Injects `EFFECT_ADJUST_PLOT_PROPERTY` into the GameEffects registry (a sample module under the `GameplayExtension` category).

## Current behavior

`AdjustPlotPropertyEffect_Init` in `AdjustPlotProperty.cpp` registers this effect (called by `OnLoad` in the plugin entry `dllmain.cpp`):

1. Resolve GameCore symbols
2. Clone the `EffectFactory<AdjustUnitProperty>` / `AdjustUnitProperty` vtable
3. Rewrite the factory `Create` / `GetTypeName`, and the UnitEffect's Apply/Remove
4. Attach the factory to `Registry<IModifierEffectFactory>::GetTypes()`
5. Apply/Remove: via `IPlotInterface` → plot dynamic property ±delta

The companion SQL is in the `B.` section of [Civ6Mod/Data/Gameplay.sql](../../../Civ6Mod/Data/Gameplay.sql) (in the same file as the sample data for the two Requirements).

## Build

```powershell
& MSBuild.exe recipes\GameCore\Civ6BasePlugin_GameCore\dll\Civ6BasePlugin_GameCore.vcxproj /t:Rebuild /m /p:Configuration=Release /p:Platform=x64
```

Output: `x64\Release\Civ6BasePlugin_GameCore.dll` (this module is already merged into that plugin).

## Files

- `AdjustPlotProperty.cpp`: implementation
- `AdjustPlotPropertyEffect.h`: `*_Init` / `*_Shutdown` declarations
- Companion SQL: the `B.` section of `Civ6Mod/Data/Gameplay.sql` (not in this directory)

## Current Loader compatibility and reload verification

- `dll/HookAPI.h` is in sync with `common/HookAPI.h` of the current Civ6DllLoader; when upgrading, copy the whole header and rebuild.
- Both Debug and Release define `CIV6_PLUGIN_GAMECORE`, and the header generates the `Civ6PluginKind` export; `OnLoad` checks the ABI first.
- Run the build command above from the repository root. On a machine where VS 2026 only has v145, append `/p:PlatformToolset=v145`; the project keeps v143 by default.
- Place the artifact in the enabled mod's `Binaries/Win64/Plugins/GameCore/`, and test XP2 with the Loader from this update.
- Within the same game process, repeatedly run "enter match → trigger the feature → save → return to main menu → load save → trigger the feature again".
- Check `Civ6DllLoader_Loader.log` for plugin load, unload queueing, release after engine unload, reload, and this plugin's success logs; a successful compile is no substitute for in-game verification.
- Fallback resolution for the current image RVA goes through `ResolveByRvaEx(..., kSectionImage)`; only old RVAs use the coordinate correction of `ResolveByRva`.
- The cloned vtable keeps the RTTI of `vtable[-1]`; the factory uses the game allocator and does not allocate when capacity is insufficient.
- Verify that the plot property is increased and restored when the effect is applied and removed respectively; run it again after loading a save.
