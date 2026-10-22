# FourRing — hook example: four-ring plot count

> English | [中文](README.md)

Makes `GameCore::City::CityHexspace::ComputePlotsFromRadius` always return **43**, so that every call site's `ComputePlotsFromRadius(2) + 18` yields exactly **61** (the total number of tiles in four rings), demonstrating "resolve a symbol by name + InstallHook".

> Note that it does not return 61 directly: call sites add 18 afterwards, and 61 + 18 = 79 would run past the 61-entry ring offset table, causing phantom plots to be counted into yields twice.

## Formula reference

| Radius | Tile count |
|------|--------|
| 0 | 1 |
| 1 | 7 |
| 2 | 19 |
| 3 | 37 |
| **4** | **61** |

The original function: `f(0)=1; f(n)=f(n-1)+6*n`, i.e. `f(n)=3n(n+1)+1`. Every call site actually uses `f(2)+18=37` (three rings), so returning `61-18=43` gives `43+18=61` (four rings).

## Build

```powershell
msbuild recipes\GameCore\FourRing\dll\FourRing.vcxproj /p:Configuration=Release /p:Platform=x64
```

Output: `x64/Release/FourRing.dll`. After wiring it into your own mod, watch `Civ6DllLoader_Loader.log`:

`FourRing: hooked ComputePlotsFromRadius -> always 43`

## Current Loader compatibility and reload verification

- `dll/HookAPI.h` is kept in sync with the current Civ6DllLoader `common/HookAPI.h`; when upgrading, copy the whole header and rebuild.
- Both Debug and Release define `CIV6_PLUGIN_GAMECORE`; the header emits the `Civ6PluginKind` export, and `OnLoad` checks the ABI first.
- Run the build command above from the repository root. On a machine with VS 2026 that only has v145, append `/p:PlatformToolset=v145`; the project keeps v143 by default.
- Place the output in an enabled Mod's `Binaries/Win64/Plugins/GameCore/` and test XP2 against the Loader from this update.
- Within a single game process, repeat "enter a match → trigger the feature → save → return to the main menu → load the save → trigger the feature again".
- Check `Civ6DllLoader_Loader.log` for plugin load, unload queueing, release after the engine unloads, reloading, and this plugin's success line; a successful compile is not a substitute for in-game verification.
