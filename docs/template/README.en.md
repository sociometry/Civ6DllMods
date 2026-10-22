# Mod Template Usage

> English | [中文](README.md)

- Get the latest shell `GameCore_XP2_Shell_FinalRelease.dll` and runtime `Civ6DllLoader.runtime.dll` and put them into `Binaries\Win64\`.
- Put your plugin DLL into **`Binaries\Win64\Plugins\Global\`** (resident plugin) or **`Binaries\Win64\Plugins\GameCore\`** (per-match plugin).
  DLLs placed directly in the `Plugins\` root are not scanned.
- When building a plugin, remember to define `CIV6_PLUGIN_GLOBAL` or `CIV6_PLUGIN_GAMECORE` in the project preprocessor (matching the subdirectory it lives in).
- Rename the template and change the GUID yourself.

## Main menu UI (optional, no compilation)

`UI\FrontEnd\FrontEndUI_YourFeature.{xml,lua}` is a sample pair of main menu screens; **no DLL needs to be compiled**.
The loader mounts them into the main menu once they are declared in `.modinfo`:

- Both files must share the same name and directory; the `FrontEndUI_` prefix cannot be changed;
- The directory structure and the `<ImportFiles>` declaration in `.modinfo` are already set up — if you rename the files, change both places together;
- The sample content is only a skeleton; for widget and Context semantics see `Civ6Docs.html` in the game directory (the ForgeUI section).

For the full explanation see [PLUGIN_GUIDE.en.md](../PLUGIN_GUIDE.en.md) chapter 7.
