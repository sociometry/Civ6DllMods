# Mod 模板使用说明

- 获取最新版薄壳 `GameCore_XP2_Shell_FinalRelease.dll` 和 runtime `Civ6DllLoader.runtime.dll`，放到 `Binaries\Win64\` 目录。
- 你的插件 DLL 放到 **`Binaries\Win64\Plugins\Global\`**（常驻插件）或 **`Binaries\Win64\Plugins\GameCore\`**（对局插件）。
  直接放在 `Plugins\` 根目录下不会被扫描到。
- 编译插件时记得在工程预处理器里定义 `CIV6_PLUGIN_GLOBAL` 或 `CIV6_PLUGIN_GAMECORE`（与所在子目录一致）。
- 自行修改名称和 GUID。

完整说明见 [PLUGIN_GUIDE.md](../PLUGIN_GUIDE.md)。
