# Civ6DllLoader-Examples

[Civ6DllLoader](https://github.com/sociometry/Civ6DllLoader) 的 DLL mod 写法示范集合。

## Layout

```text
recipes/
  Global/             Global 插件示范（Plugins/Global/，进程内常驻）
  GameCore/           GameCore 插件示范（Plugins/GameCore/，随对局加载/卸载）
docs/                 作者指南与 mod 模板
```

每个 recipe：

```text
recipes/<域>/<Name>/
  README.md
  dll/               VS 插件工程
  Civ6Mod/           可选：mod 工程
```

本仓库**不提供**可直接使用的完整 mod 。自行组装见 `docs/`。

## Recipes

| Recipe | 说明 |
|--------|------|
| `FourRing` | 实战 hook（`ComputePlotsFromRadius`）：恒返回 61，验证按名取符号 + `InstallHook` |
| `AdjustPlotPropertyEffect` | 在 runtime 中注入 `EFFECT_ADJUST_PLOT_PROPERTY` 的脚手架示范 |
| `NewRequirements` | 新增两个自定义 Requirement：玩家属性阈值（`REQUIREMENT_PLAYER_PROPERTY_MATCHES`）与地块道路类型（`REQUIREMENT_PLOT_ROUTE_TYPE_MATCHES`） |
| `HookExe` | Global exe hook：观察 DX11 exe 的 `ResourceList.AddResource`，调用原函数并累计调用次数 |
| `PatchExe` | Global 字节补丁：改写 DX11 exe 容量判断的立即数（`8192` → `256`），演示指令校验与恢复 |
| `ApiSmokeTest` | HookAPI 全量接口冒烟测试（41 组，A/B/C/D 四阶段时序） |
| `Civ6BasePlugin_Global` | 基础插件集 Global 侧：`UnlockModelLimit`（解除模型/材质上限）+ `ModConfig`（全局 Lua 配置接口，挂到引擎主 Lua state） |
| `Civ6BasePlugin_GameCore` | 基础插件集 GameCore 侧：`GreatWorkOptimizer` / `GreatWorkTypeLookup` / `GreatWorkYieldStack`（BugFix）+ `GreatWorkAPI` / `ModifierManagerAPI`（Lua API） |

### Civ6BasePlugin 的两段式拆分

原单工程 `Civ6BasePlugin` 已拆为两个工程，分属两类插件域，**生命周期与可调用接口都不同**（详见 [`docs/PLUGIN_GUIDE.md` §3](docs/PLUGIN_GUIDE.md)）：

| 工程 | 域 | 部署目录 | 生命周期 | 内容 |
|------|----|----------|----------|------|
| `Civ6BasePlugin_Global` | Global | `Plugins/Global/` | 游戏启动早期（Phase A）加载，进程内常驻、永不卸载 | exe 域：`UnlockModelLimit`；Lua 域：`ModConfig`（引擎主 state） |
| `Civ6BasePlugin_GameCore` | GameCore | `Plugins/GameCore/` | 每次进对局（Phase B）加载，退局 / 回主菜单 / 重载对局时卸载 | 对局玩法域：3 个 BugFix + 2 个 Lua API 扩展 |

拆分的原因之一是两个域的**接口门禁**不同：`GetPlayer`、`RegisterGameEvent`、`TriggerGameEvent`、`RegisterGameCoreScriptForGamePlay`、`PushLuaMethods` 等 GameCore 专属入口，在定义了 `CIV6_PLUGIN_GLOBAL` 的工程里**根本不存在**，跨域调用会在编译期直接失败。

### 新增能力（2026-10）

- **`ModConfig`**：给 mod 作者的键值持久化接口，数据落在
  `Documents\My Games\Sid Meier's Civilization VI\ModConfigs\<名字>.json`
  —— `<名字>` 就是调用时传的第一个参数（如 `ModConfig.Set("MyMod", ...)` 对应
  `ModConfigs\MyMod.json`），只做文件名清洗，不读 `.modinfo`。
  实现在 Global 侧 `APIExtension/ModConfig`，用 `RegisterGlobalScript` 注册一次 ——
  该入口的语义就是"注册全局可用的接口"，loader 会挂到主菜单与对局的每个脚本环境。
  用法见 [`docs/MODCONFIG.md`](docs/MODCONFIG.md)。
- **调研与路线图**：[`docs/RESEARCH_NEW_FEATURES.md`](docs/RESEARCH_NEW_FEATURES.md)（联网调研，
  1515 行，含证据链接与 15 个功能的 V/D/F 评分）、[`docs/ROADMAP.md`](docs/ROADMAP.md)（从报告收敛出的排期）。
- **GameCore 侧 5 个模块全部恢复参与编译**：去掉已删除的 `RegisterGameCoreReady` /
  `RegisterGameCoreTeardown` 代际回调与改名前的 `EditPlayer`，取址统一改为
  "符号名 + 唯一 AOB 兜底 + 前导字节校验"，详见
  [`recipes/GameCore/Civ6BasePlugin_GameCore/README.md`](recipes/GameCore/Civ6BasePlugin_GameCore/README.md)。

### 本地验证脚本

```bash
bash tools/build_global.sh          # 构建 Global 工程
bash tools/build_gamecore.sh        # 构建 GameCore 工程
python tools/aob_scan.py            # 用当前 GameCore DLL 复核所有 AOB 的唯一性
python tools/verify_stack_semantics.py   # 核对 ModConfig 的栈操作与"不含内嵌 Lua"
cmd //c tools\minijson_test\build.cmd && ./tools/minijson_test/minijson_test.exe
cmd //c tools\modconfig_test\build.cmd && ./tools/modconfig_test/modconfig_test.exe

# 部署到 FourRing（构建 + 复制 + 校验）
bash tools/deploy_fourring.sh
bash tools/deploy_fourring.sh --check     # 只校验现有部署

# 单独跑的校验工具
python tools/verify_deployment.py         # 插件 PE 位数 / 导出表 / 架构
python tools/check_runtime_api.py         # runtime 是否含 HookAPI.h 的全部入口
python tools/check_mod_lua.py "<mod目录>"  # Mod 里 Lua + XML 的语法检查
```

### 部署与自检

部署位置、自检面板、对局内自检、失败排查表都在
[`docs/DEPLOY_FOURRING.md`](docs/DEPLOY_FOURRING.md)。

## Build

1. 先构建 **Civ6DllLoader**（薄壳 + runtime）。
2. 然后构建 recipe 的 dll：可在仓库根打开 `Recipes.sln` 一次构建，也可单独 MSBuild 某个 `.vcxproj`。
3. 产物默认在仓库根 `x64/Release/<Name>.dll`——包括拆分后的两个 Civ6BasePlugin 工程，它们同样统一输出到仓库根，不再各自落在工程目录下。
4. 按插件域放入 mod：Global 插件进 `Binaries/Win64/Plugins/Global/`，GameCore 插件进 `Binaries/Win64/Plugins/GameCore/`（直接放在 `Plugins/` 根目录下的 DLL 会被忽略）。
5. 把 DLL 与 `Civ6Mod/` 素材接到你自己的 mod（参见 `docs/PLUGIN_GUIDE.md`、`docs/template/`）。

升级 `HookAPI.h`：从 Civ6DllLoader 发行包再复制进各 `recipes/*/*/dll/`。
