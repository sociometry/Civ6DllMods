# Civ6DllLoader-Examples

> English | [中文](README.md)

A collection of DLL mod examples for [Civ6DllLoader](https://github.com/sociometry/Civ6DllLoader).

This repository **does not** ship ready-to-play mods. It only provides plugin projects and documentation you can study and copy. To assemble your own, see the [plugin development guide](docs/PLUGIN_GUIDE.en.md).

---

## Example projects

Plugins fall into two categories by lifecycle; **both the deployment directory and the available interfaces differ**:

- **Global** — loaded early during game startup, resident for the process lifetime, never unloaded. Deployed to `Binaries/Win64/Plugins/Global/`
- **GameCore** — loaded on entering a match, unloaded along with the image when leaving the match / returning to the main menu. Deployed to `Binaries/Win64/Plugins/GameCore/`

### Global domain

| Project | Description |
| --- | --- |
| [Civ6BasePlugin](recipes/Global/Civ6BasePlugin/README.md) | Base plugin set. `UnlockModelLimit` removes the engine's model / material count limits, enabling larger mod setups; `ModConfig` gives mod authors Lua interfaces for reading and writing configuration |

### GameCore domain

| Project | Description |
| --- | --- |
| [Civ6BasePlugin_GameCore](recipes/GameCore/Civ6BasePlugin_GameCore/README.md) | Base plugin set. **Fixes 7 vanilla defects** (Great Work assignment stutter / Great Work types and slots cannot be added / yield bonuses do not stack / changing Great Work slot counts swallows already-placed Great Works / Great Works do not trigger attached Modifiers / `InitUnitValidAdjacentHex` returns nil / units with more than 100 HP cannot be killed) + **adds 3 groups of Lua interfaces** (Great Works / Modifiers / unit levels) + **2 custom Requirements** (player property threshold / plot road type) + **1 custom Effect** (`EFFECT_ADJUST_PLOT_PROPERTY`) |
| [FourRing](recipes/GameCore/FourRing/README.md) | The smallest real hook: makes `ComputePlotsFromRadius` always return 43 (the call site adds 18 to get 61), demonstrating symbol lookup by name + `InstallHook` |

> Every project directory carries a `README.md` explaining what it changes, how the target was located, and which pitfalls were hit. The table links go straight there.

---

## Documentation

| Document | Content |
| --- | --- |
| [PLUGIN_GUIDE.en.md](docs/PLUGIN_GUIDE.en.md) | **The main document.** Plugin project configuration, address resolution (symbol name + AOB fallback), Hook / Patch, Lua extensions, asmjit-generated detours, troubleshooting table, and **§5.9 the scope in which prologue checks are valid** |
| [DX12_UNLOCKMODELLIMIT_ANALYSIS.md](docs/DX12_UNLOCKMODELLIMIT_ANALYSIS.md) | Analysis of porting UnlockModelLimit from DX11 to DX12 (IDA evidence from a live binary). **Chinese only** |
| [template/](docs/template/) | Mod directory template: `.modinfo`, Data SQL, UI XML/Lua |

---

## Build

1. Build [Civ6DllLoader](https://github.com/sociometry/Civ6DllLoader) first (thin shell + runtime).
2. Then build this repository's plugin projects. You can open `Recipes.sln` at the repo root to build everything at once, or MSBuild a single `.vcxproj`:

   ```powershell
   & 'MSBuild.exe' `
     'recipes\Global\Civ6BasePlugin\dll\Civ6BasePlugin_Global.vcxproj' `
     /p:Configuration=Release /p:Platform=x64
   ```

3. Artifacts are written uniformly to `x64/Release/<Name>.dll` at the repository root.
4. Place them into the mod's `Plugins/Global/` or `Plugins/GameCore/` according to the plugin domain. **DLLs placed directly in the `Plugins/` root are ignored.**

Common project requirements: `x64`, C++20, `/utf-8` (the repository convention is UTF-8 without BOM); projects that reference asmjit also need `/MT` and `ASMJIT_STATIC`.

> **A version resource is mandatory.** Put a `Version.rc` in every plugin project with just these three lines (the recipes in this repo already have one — copy it):
>
> ```rc
> VS_VERSION_INFO VERSIONINFO
>  FILEVERSION 1,0,0,0
> BEGIN
> END
> ```
>
> and add `<ResourceCompile Include="Version.rc" />` to the `.vcxproj`.
> To bump the version, change only the `FILEVERSION` line. The loader **refuses to load any DLL whose version it cannot read**;
> when several mods ship a plugin with the same name, only the **highest version** is loaded. See [plugin guide §2.2](docs/PLUGIN_GUIDE.en.md).

`HookAPI.h` must match the runtime version — copy it from the Civ6DllLoader release package into each `recipes/*/*/dll/`.

---

## Tools

The repository ships one tool: `tools/rva_to_symbol.exe`.

It embeds the loader's symbol DB (a single file, embedded at compile time, no extra environment needed). Give it an RVA and it looks up the symbol name:

```text
tools\rva_to_symbol.exe -gamecore 0x3A5A60          # GameCore_XP2_FinalRelease.dll
tools\rva_to_symbol.exe -dx11     0x4D0D70          # CivilizationVI.exe
tools\rva_to_symbol.exe -dx12     0x37AB0           # CivilizationVI_DX12.exe
tools\rva_to_symbol.exe --list                     # list available images and symbol DB files
tools\rva_to_symbol.exe -gamecore 0x3A5A60 --demangled   # one column only
```

**The input is an RVA, not an absolute address** — what you get in IDA is a VA, so subtract the image base first. The three images have separate RVA spaces, so the selector must be explicit.

Once you have the symbol name, write it as `HookName("GameCore::…")`; cross-version correction is handled by `PdataMatch`. See [plugin guide §5.8](docs/PLUGIN_GUIDE.en.md).

---

## Directory layout

```text
recipes/
  Global/             Global plugin projects (Plugins/Global/, resident for the process)
  GameCore/           GameCore plugin projects (Plugins/GameCore/, loaded per match)
  <domain>/<Name>/
    README.md         what this project changes, how the target was located, pitfalls hit
    dll/              VS plugin project
    Civ6Mod/          optional: mod project
docs/                 developer guide, focused analyses, mod template
tools/                build / verify / deploy scripts
```

---

## Contributors

| Contributor | What they built |
| --- | --- |
| [Aws63140](https://github.com/Aws63140) | `Player:GetGovernors():SetAvailableGovPoints(n)` ([GovernorAPI](recipes/GameCore/Civ6BasePlugin_GameCore/dll/APIExtension/GovernorAPI/README.md))<br>`Plot:ChangeAppeal(n)` ([PlotAPI](recipes/GameCore/Civ6BasePlugin_GameCore/dll/APIExtension/PlotAPI/README.md)) |
| [sociometry](https://github.com/sociometry) | Custom Requirements (`REQUIREMENT_PLAYER_PROPERTY_MATCHES` / `REQUIREMENT_PLOT_ROUTE_TYPE_MATCHES`)<br>Custom Effect (`EFFECT_ADJUST_PLOT_PROPERTY`)<br>The [FourRing](recipes/GameCore/FourRing/README.md) project<br>See [GameplayExtension](recipes/GameCore/Civ6BasePlugin_GameCore/dll/GameplayExtension/) |
| [Sumcam-L](https://github.com/Sumcam-L) | GreatWorkAPI, ModifierManagerAPI, UnitLevelAPI, and the seven Great Work / unit bug fixes |
