# Mod 模板使用说明

> 中文 | [English](README.en.md)

- 获取最新版薄壳 `GameCore_XP2_Shell_FinalRelease.dll` 和 runtime `Civ6DllLoader.runtime.dll`，放到 `Binaries\Win64\` 目录。
- 你的插件 DLL 放到 **`Binaries\Win64\Plugins\Global\`**（常驻插件）或 **`Binaries\Win64\Plugins\GameCore\`**（对局插件）。
  直接放在 `Plugins\` 根目录下不会被扫描到。
- 编译插件时记得在工程预处理器里定义 `CIV6_PLUGIN_GLOBAL` 或 `CIV6_PLUGIN_GAMECORE`（与所在子目录一致）。
- 自行修改名称和 GUID。

## 主菜单 UI（可选，不需要编译）

`UI\FrontEnd\FrontEndUI_YourFeature.{xml,lua}` 是一对示例主菜单界面，**不需要编译任何 DLL**，
由 Loader 在 `.modinfo` 声明后自动挂进主菜单：

- 两个文件必须同名同目录，前缀 `FrontEndUI_` 不能改；
- 目录结构与 `.modinfo` 里的 `<ImportFiles>` 声明已经写好，改文件名时两处要同步改；
- 示例内容只是骨架，控件与 Context 的语义见游戏目录下的 `Civ6Docs.html`（ForgeUI 部分）。

完整说明见 [PLUGIN_GUIDE.md](../PLUGIN_GUIDE.md) 第 7 章。
