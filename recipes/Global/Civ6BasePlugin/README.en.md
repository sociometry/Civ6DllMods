# Civ6BasePlugin (Global side)

> English | [中文](README.md)

The Global side of Civilization VI's "base plugin pack", running on the HookAPI plugin architecture of the [Civ6DllLoader] runtime.

- **Plugin category**: Global (macro `CIV6_PLUGIN_GLOBAL`), entry point `OnLoad(HookContext* ctx)`
- **Lifecycle**: resident for the lifetime of the process, present both in the main menu and during a match
- **Deployment location**: `<Mod>/Binaries/Win64/Plugins/Global/Civ6BasePlugin_Global.dll`
- **Supported images**: DX11 `CivilizationVI.exe` and DX12 `CivilizationVI_DX12.exe`, discriminated automatically at runtime

Sibling project: [GameCore side](../../GameCore/Civ6BasePlugin_GameCore/README.md) (loaded only when a match starts, unloaded as soon as it ends).

---

## Module list

Every module has its own README explaining what problem it solves, how it is implemented, and which pitfalls were hit.

### BugFix — fixes vanilla defects

| Module | What problem it solves |
| --- | --- |
| [UnlockModelLimit](dll/BugFix/UnlockModelLimit/README.md) | The engine's model / material count limits: vanilla already errors out or even fails to render once assets pile up; with this installed, a much larger mod scale is supported |

### APIExtension — adds Lua interfaces

| Module | Which interface it adds |
| --- | --- |
| [ModConfig](dll/APIExtension/ModConfig/README.md) | A set of out-of-the-box key-value persistence Lua APIs, used to store player settings and progress data |

---

## Directory structure

```
recipes/Global/Civ6BasePlugin/
├── README.md
└── dll/
    ├── Civ6BasePlugin_Global.vcxproj
    ├── Civ6BasePlugin_Global.vcxproj.filters
    ├── HookAPI.h
    ├── framework.h / pch.h / pch.cpp
    ├── dllmain.cpp                          # OnLoad: ABI defence + RequireAll + module Init
    ├── include/asmjit/                      # vendored asmjit (source headers)
    ├── include/Zydis/ , include/Zycore/     # vendored disassembler libraries
    ├── lib/asmjit.lib , Zydis.lib , Zycore.lib
    ├── BugFix/UnlockModelLimit/             + README.md
    │   ├── UnlockModelLimit.h               # entry declaration
    │   ├── UnlockModelLimit_Common.h        # shared utilities (Patch / decode / hook install / site resolution)
    │   ├── UnlockModelLimit.cpp             # DX11 branch
    │   └── UnlockModelLimit_Dx12.cpp        # DX12 branch
    └── APIExtension/ModConfig/              + README.md
        ├── MiniJson.h                       # minimal JSON container / parse / serialize
        ├── ModConfigCore.h                  # file IO + Lua binding (header-only)
        ├── ModConfig.h
        └── ModConfig.cpp                    # entry: attached to the engine main state
```

---

## Build

```powershell
& 'E:\VisualStudio\Products\MSBuild\Current\Bin\amd64\MSBuild.exe' `
  'recipes\Global\Civ6BasePlugin\dll\Civ6BasePlugin_Global.vcxproj' `
  /p:Configuration=Release /p:Platform=x64
```

Output: `x64/Release/Civ6BasePlugin_Global.dll`.

Project configuration (Release | x64): `CIV6_PLUGIN_GLOBAL`, `ASMJIT_STATIC`, `ZYDIS_STATIC_BUILD`,
C++20, `/MT` (consistent with the vendored `*.lib`), `/utf-8`.

---

## Deployment

```
<Mod>/Binaries/Win64/Plugins/Global/Civ6BasePlugin_Global.dll
```

---

## Logging

The runtime log (`%LOCALAPPDATA%\Firaxis Games\Sid Meier's Civilization VI\Logs\Civ6DllLoader_Loader.log`)
should contain:

```
UnlockModelLimit: exe image = DX12
[DX12] hooks installed: %d/2
UnlockModelLimit[DX12]: installed (%zu patches)
[ModConfig] registered; dir=...\ModConfigs
```

> `%d` / `%zu` are runtime values inside format strings, not fixed numbers; the set of hook sites varies with the exe version.
> The corresponding line of the DX11 branch is `UnlockModelLimit: installed (%zu patches, %d/4 hooks)`.

If any one of them is missing, or `redirects incomplete` appears, the hook-site layout of that exe version has changed and the patches are skipped as a whole
(the feature does not take effect, but memory is left untouched).

> See each module's README for its specific verification method and log samples.

---

## Dependencies

The third-party libraries are vendored under `dll/include` and `dll/lib`:

| Library | Version | How to tell |
| --- | --- | --- |
| asmjit | **1.17** | `ASMJIT_ABI_NAMESPACE v1_17` in `include/asmjit/core/api-config.h`; the symbols exported by `lib/asmjit.lib` carry `@v1_17@` |
| Zydis | **4.1.0** | `ZYDIS_VERSION 0x0004000100000000` in `include/Zydis/Zydis.h` |
| Zycore | **1.5.2** | `ZYCORE_VERSION 0x0001000500020000` in `include/Zycore/Zycore.h` |

(ModConfig does not depend on them; it is pure Win32 plus the bundled MiniJson.)
When upgrading these libraries, all three version-table entries must change together, and the `ASMJIT_STATIC` / `ZYDIS_STATIC_BUILD` macros must be re-checked.

---

## Notes

- **Version binding**: hook-site RVAs target the current exe build. After a game update, first check whether the AOB fallback takes effect;
  a "switched to AOB" entry in the log means the main path has drifted, and the RVAs and AOBs should be updated.
- **Memory patches**: only process memory is modified; no file on disk is touched.
- **Extended arrays**: located within imagebase ±2GB, matching the disp32 addressing of the engine's original arrays.
- **`/utf-8` is required**: the repository `.editorconfig` mandates UTF-8 without BOM, while MSVC by default parses
  Chinese comments with the system code page and errors out.

---

## References

- Repository overview: [`README.md`](../../../README.md)
- Plugin development guide: [`docs/PLUGIN_GUIDE.md`](../../../docs/PLUGIN_GUIDE.md)
- The patch logic originates from **DllProxy** (the `version.dll` proxy approach), later reworked into the HookAPI plugin form

[Civ6DllLoader]: https://github.com/sociometry/Civ6DllLoader
