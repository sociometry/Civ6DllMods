# Civ6DllLoader-Examples

> 中文 | [English](README.en.md)

[Civ6DllLoader](https://github.com/sociometry/Civ6DllLoader) 的 DLL mod 写法示范集合。

本仓库**不提供**可直接使用的完整 mod，只提供可参考、可复制的插件工程与文档。自行组装见 [插件开发指南](docs/PLUGIN_GUIDE.md)。

---

## 示例工程

插件按生命周期分两类，**部署目录与可用接口都不同**：

- **Global** —— 游戏启动早期加载，进程常驻、永不卸载，部署到 `Binaries/Win64/Plugins/Global/`
- **GameCore** —— 每次进入对局加载，退局 / 回主菜单时随映像一起卸载，部署到 `Binaries/Win64/Plugins/GameCore/`

### Global 域

| 工程 | 说明 |
| --- | --- |
| [Civ6BasePlugin](recipes/Global/Civ6BasePlugin/README.md) | 基础插件集。`UnlockModelLimit` 解除引擎的模型 / 材质数量上限，支撑更大的模组规模；`ModConfig` 给模组作者提供配置读写的 Lua 接口 |

### GameCore 域

| 工程 | 说明 |
| --- | --- |
| [Civ6BasePlugin_GameCore](recipes/GameCore/Civ6BasePlugin_GameCore/README.md) | 基础插件集。**修 7 个原版缺陷**（巨作分配卡顿 / 巨作类型与槽位无法新增 / 产出加成不叠加 / 改巨作槽数会吞掉已放置的巨作 / 巨作不触发挂载的 Modifier / `InitUnitValidAdjacentHex` 返回 nil / 单位血量超 100 打不死）+ **补 3 组 Lua 接口**（巨作 / 修饰符 / 单位等级）+ **2 个自定义 Requirement**（玩家属性阈值 / 地块道路类型）+ **1 个自定义 Effect**（`EFFECT_ADJUST_PLOT_PROPERTY`） |
| [FourRing](recipes/GameCore/FourRing/README.md) | 最小实战 hook：`ComputePlotsFromRadius` 固定返回 43（调用点再加 18 得 61），验证按名取符号 + `InstallHook` |

> 目录里每个工程都带 `README.md`，讲清楚它改了什么、怎么定位目标、踩过哪些坑。上表的链接直达。

---

## 文档

| 文档 | 内容 |
| --- | --- |
| [PLUGIN_GUIDE.md](docs/PLUGIN_GUIDE.md) | **主文档**。插件工程配置、取址模型（符号名 + AOB 兜底）、Hook / Patch、Lua 扩展、asmjit 生成 detour、排查表、**§5.9 函数序言校验的适用范围** |
| [DX12_UNLOCKMODELLIMIT_ANALYSIS.md](docs/DX12_UNLOCKMODELLIMIT_ANALYSIS.md) | UnlockModelLimit 从 DX11 移植到 DX12 的差异分析（IDA 实机取证） |
| [template/](docs/template/) | mod 目录模板：`.modinfo`、Data SQL、UI XML/Lua |

---

## 构建

1. 先构建 [Civ6DllLoader](https://github.com/sociometry/Civ6DllLoader)（薄壳 + runtime）。
2. 再构建本仓库的插件工程。可在仓库根打开 `Recipes.sln` 一次全建，也可单独 MSBuild 某个 `.vcxproj`：

   ```powershell
   & 'MSBuild.exe' `
     'recipes\Global\Civ6BasePlugin\dll\Civ6BasePlugin_Global.vcxproj' `
     /p:Configuration=Release /p:Platform=x64
   ```

3. 产物统一输出到仓库根 `x64/Release/<Name>.dll`。
4. 按插件域放进 mod 的 `Plugins/Global/` 或 `Plugins/GameCore/`。**直接放在 `Plugins/` 根目录下的 DLL 会被忽略。**

工程通用要求：`x64`、C++20、`/utf-8`（仓库约定源码为 UTF-8 无 BOM）；引用了 asmjit 的工程还需 `/MT` 与 `ASMJIT_STATIC`。

> **必须带版本资源。** 每个插件工程放一个 `Version.rc`，内容就三行（本仓库各 recipe 里已有，照抄即可）：
>
> ```rc
> VS_VERSION_INFO VERSIONINFO
>  FILEVERSION 1,0,0,0
> BEGIN
> END
> ```
>
> 并在 `.vcxproj` 里加 `<ResourceCompile Include="Version.rc" />`。
> 改版本只改 `FILEVERSION` 这一行。Loader 扫描时**读不到版本的 DLL 一律不加载**；
> 多个 Mod 带同名插件时只加载**版本最高**的那份。详见 [插件开发指南 §2.2](docs/PLUGIN_GUIDE.md)。

`HookAPI.h` 需要与运行时版本配套 —— 从 Civ6DllLoader 发行包复制进各 `recipes/*/*/dll/`。

---

## 工具

仓库只带一个工具：`tools/rva_to_symbol.exe`。

它内嵌了 Loader 的符号库（单文件，编译期嵌入，不需要额外环境），输入 RVA 即可反查符号名：

```text
tools\rva_to_symbol.exe -gamecore 0x3A5A60          # GameCore_XP2_FinalRelease.dll
tools\rva_to_symbol.exe -dx11     0x4D0D70          # CivilizationVI.exe
tools\rva_to_symbol.exe -dx12     0x37AB0           # CivilizationVI_DX12.exe
tools\rva_to_symbol.exe --list                     # 查看可用映像与符号库文件名
tools\rva_to_symbol.exe -gamecore 0x3A5A60 --demangled   # 只取一栏
```

**输入是 RVA，不是绝对地址** —— IDA 里拿到的是 VA，要先减去映像基址。三个映像的 RVA 空间互不相通，所以必须显式给选择符。

拿到符号名后写成 `HookName("GameCore::…")`，跨版本交给 `PdataMatch` 矫正。详见 [插件开发指南 §5.8](docs/PLUGIN_GUIDE.md)。

---

## 目录结构

```text
recipes/
  Global/             Global 插件工程（Plugins/Global/，进程常驻）
  GameCore/           GameCore 插件工程（Plugins/GameCore/，随对局加载）
  <域>/<Name>/
    README.md         该工程改了什么、怎么定位目标、踩过哪些坑
    dll/              VS 插件工程
    Civ6Mod/          可选：mod 工程
docs/                 开发指南、专题分析、mod 模板
tools/                构建 / 校验 / 部署脚本
```

---

## 贡献者

| 贡献者 | 负责的功能 |
| --- | --- |
| [Aws63140](https://github.com/Aws63140) | `Player:GetGovernors():SetAvailableGovPoints(n)`（[GovernorAPI](recipes/GameCore/Civ6BasePlugin_GameCore/dll/APIExtension/GovernorAPI/README.md)）<br>`Plot:ChangeAppeal(n)`（[PlotAPI](recipes/GameCore/Civ6BasePlugin_GameCore/dll/APIExtension/PlotAPI/README.md)） |
| [sociometry](https://github.com/sociometry) | 自定义 Requirement（`REQUIREMENT_PLAYER_PROPERTY_MATCHES` / `REQUIREMENT_PLOT_ROUTE_TYPE_MATCHES`）<br>自定义 Effect（`EFFECT_ADJUST_PLOT_PROPERTY`）<br>[FourRing](recipes/GameCore/FourRing/README.md) 工程<br>见 [GameplayExtension](recipes/GameCore/Civ6BasePlugin_GameCore/dll/GameplayExtension/) |
| [Sumcam-L](https://github.com/Sumcam-L) | Civ6DllLoader 插件工程与文档、GreatWorkAPI、ModifierManagerAPI、UnitLevelAPI、7 个巨作/单位相关 BugFix |
