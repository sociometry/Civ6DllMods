# Civ6DllLoader-Examples

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
| [Civ6BasePlugin](recipes/Global/Civ6BasePlugin/README.md) | 基础插件集。`UnlockModelLimit` 解除渲染资源上限（IB/VB 8192→16384，SPI 16384→32768）；`ModConfig` 提供全局 Lua 键值持久化接口 |
| [HookExe](recipes/Global/HookExe/README.md) | exe 域 hook 示范：观察 `ResourceList.AddResource` 并累计调用次数 |
| [PatchExe](recipes/Global/PatchExe/README.md) | exe 域字节补丁示范：改写容量判断的立即数，含指令校验与恢复 |

### GameCore 域

| 工程 | 说明 |
| --- | --- |
| [Civ6BasePlugin_GameCore](recipes/GameCore/Civ6BasePlugin_GameCore/README.md) | 基础插件集。4 个 BugFix（巨作分配优化 / 类型解析 / 产出叠加 / **单位伤害上限**）+ 3 个 Lua API 扩展（巨作 / 修饰符 / **单位等级**） |
| [FourRing](recipes/GameCore/FourRing/README.md) | 最小实战 hook：`ComputePlotsFromRadius` 固定返回 43（调用点再加 18 得 61），验证按名取符号 + `InstallHook` |
| [NewRequirements](recipes/GameCore/NewRequirements/README.md) | 新增自定义 Requirement：玩家属性阈值、地块道路类型 |
| [AdjustPlotPropertyEffect](recipes/GameCore/AdjustPlotPropertyEffect/README.md) | 注入 `EFFECT_ADJUST_PLOT_PROPERTY` 的脚手架示范 |

> 目录里每个工程都带 `README.md`，讲清楚它改了什么、怎么定位目标、踩过哪些坑。上表的链接直达。

---

## 文档

| 文档 | 内容 |
| --- | --- |
| [PLUGIN_GUIDE.md](docs/PLUGIN_GUIDE.md) | **主文档**。插件工程配置、取址模型（符号名 + AOB 兜底）、Hook / Patch、Lua 扩展、asmjit 生成 detour、排查表 |
| [DX12_UNLOCKMODELLIMIT_ANALYSIS.md](docs/DX12_UNLOCKMODELLIMIT_ANALYSIS.md) | UnlockModelLimit 从 DX11 移植到 DX12 的差异分析（IDA 实机取证） |
| [template/](docs/template/) | mod 目录模板：`.modinfo`、Data SQL、UI XML/Lua |

---

## 构建

1. 先构建 [Civ6DllLoader](https://github.com/sociometry/Civ6DllLoader)（薄壳 + runtime）。
2. 再构建本仓库的插件工程。可在仓库根打开 `Recipes.sln` 一次全建，也可单独 MSBuild 某个 `.vcxproj`。
3. 产物统一输出到仓库根 `x64/Release/<Name>.dll`。
4. 按插件域放进 mod 的 `Plugins/Global/` 或 `Plugins/GameCore/`。**直接放在 `Plugins/` 根目录下的 DLL 会被忽略。**

工程通用要求：`x64`、C++20、`/utf-8`（仓库约定源码为 UTF-8 无 BOM）；引用了 asmjit 的工程还需 `/MT` 与 `ASMJIT_STATIC`。

`HookAPI.h` 需要与运行时版本配套 —— 从 Civ6DllLoader 发行包复制进各 `recipes/*/*/dll/`。

---

## 工具

```bash
# 构建
bash tools/build_global.sh                 # 构建 Global 工程
bash tools/build_gamecore.sh               # 构建 GameCore 工程

# 校验
python tools/aob_scan.py                   # 复核所有 AOB 特征码在目标映像内是否唯一
python tools/verify_deployment.py          # 检查插件 PE 位数 / 导出表 / 插件类别
python tools/check_runtime_api.py          # 检查 runtime 是否包含 HookAPI.h 的全部入口
python tools/check_mod_lua.py "<mod目录>"   # 检查 mod 里 Lua 与 XML 的语法
python tools/verify_stack_semantics.py     # 核对 ModConfig 的栈操作

# 部署到 FourRing（构建 + 复制 + 校验）
bash tools/deploy_fourring.sh
bash tools/deploy_fourring.sh --check      # 只校验现有部署
```

另有 `tools/rva_to_symbol.exe`：输入 RVA 反查 Loader 内嵌符号库里的符号名（写 `HookName` 时用）。

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