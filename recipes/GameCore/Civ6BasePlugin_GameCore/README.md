# Civ6BasePlugin_GameCore

Civilization VI 的 **GameCore 生命周期插件工程**，跑在 [Civ6DllLoader] runtime 的 **HookAPI** 插件架构上。

- **插件类别**：GameCore（宏 `CIV6_PLUGIN_GAMECORE`），入口 `OnLoad(HookContext* ctx)`
- **生命周期**：每次**进入对局**加载，**退出对局 / 回主菜单 / 重载对局**时随整个 GameCore 映像一起卸载
- **部署位置**：`<Mod>/Binaries/Win64/Plugins/GameCore/Civ6BasePlugin_GameCore.dll`
- **构建**：MSBuild（Release | x64，C++20，`/utf-8`，`/MT`）

姊妹工程：[Global 侧](../../Global/Civ6BasePlugin/README.md)（常驻进程，含 ModConfig 与渲染上限解锁）。

---

## 模块清单

每个模块都有一份独立 README，讲清楚它修的是什么缺陷、怎么定位、踩过哪些坑。

### BugFix —— 修原版缺陷

| 模块 | 解决什么问题 |
| --- | --- |
| [GreatWorkOptimizer](dll/BugFix/GreatWorkOptimizer/README.md) | 大后期 AI 计算巨作分配时的卡顿（O(G³) 重算） |
| [GreatWorkTypeLookup](dll/BugFix/GreatWorkTypeLookup/README.md) | 模组新增的巨作类型 / 巨作槽类型无法被引擎识别 |
| [GreatWorkYieldStack](dll/BugFix/GreatWorkYieldStack/README.md) | 同种巨作来自多个来源的产出加成不叠加，只有一条生效 |
| [GreatWorkPreserve](dll/BugFix/GreatWorkPreserve/README.md) | 改变额外巨作槽数量时，该建筑已放置的巨作全部丢失 |
| [GreatWorkModifierAttach](dll/BugFix/GreatWorkModifierAttach/README.md) | 巨作创建后不触发挂在它身上的 Modifier（伟人激活与 Lua 接口两条路） |
| [InitUnitReturn](dll/BugFix/InitUnitReturn/README.md) | `InitUnitValidAdjacentHex` 返回 nil，拿不到建出来的单位 |
| [UnitDamageCap](dll/BugFix/UnitDamageCap/README.md) | 单位生命值超过 100 后伤害打不死 |

### APIExtension —— 补 Lua 接口

| 模块 | 补什么接口 |
| --- | --- |
| [GreatWorkAPI](dll/APIExtension/GreatWorkAPI/README.md) | `GreatWorkManager.AddGreatWork` / `AddGreatWorkToCity`（支持同种巨作多份） |
| [ModifierManagerAPI](dll/APIExtension/ModifierManagerAPI/README.md) | `unit:AttachModifierByID` / `ModifierManager.DetachModifierByID` / `GetCount` |
| [UnitLevelAPI](dll/APIExtension/UnitLevelAPI/README.md) | `unit:GetExperience():SetLevel(n)` |

### GameplayExtension —— 新增引擎本来没有的游戏对象

| 模块 | 注册什么 |
| --- | --- |
| [CustomRequirement](dll/GameplayExtension/CustomRequirement/README.md) | 两个自定义 **Requirement**：`REQUIREMENT_PLAYER_PROPERTY_MATCHES`（玩家动态属性阈值）、`REQUIREMENT_PLOT_ROUTE_TYPE_MATCHES`（地块道路类型）。前者装 4 个钩，后者装 3 个 |
| [AdjustPlotPropertyEffect](dll/GameplayExtension/AdjustPlotPropertyEffect/README.md) | 一个自定义 **Effect**：`EFFECT_ADJUST_PLOT_PROPERTY`（改地块动态属性）。不改钩子，靠克隆 `AdjustUnitProperty` 的工厂与效果虚表 |

> 这两类做的是**同一件事的两面**：`Requirement` 决定「条件是否成立」，`Effect` 决定「成立后做什么」。
> 两者都要往 `GameEffects` 的注册表里注入自定义对象，所以归在同一个 `GameplayExtension` 下。
> 配套 SQL 示范在 `Civ6Mod/Data/Gameplay.sql`。

---

## 兼容性取址

所有目标统一走 `HookName(符号名)` 主路径 + 唯一 AOB 兜底（见 `dll/Common/GameCoreResolve.h`）。
裸 RVA 在符号库与当前映像对不上时会被 Loader **原样透传**，等于把钩子装到错误的函数上。

> **不做函数序言校验**（Prologue Check）。函数入口会被别的插件 hook 改写成
> `E9 xx xx xx xx`，此时「比对原始入口字节」必然失败，而失败原因与「游戏更新」无法区分。
> 相关条件与例外见 [插件开发指南 §5.9](../../../docs/PLUGIN_GUIDE.md)。

符号名一律用 **demangled** 形式（`tools/rva_to_symbol.exe` 反查得到），可读性最好：

```bash
tools\rva_to_symbol.exe -gamecore 0x3A5A60 --demangled
```

> 输入是 **RVA 不是 VA**：IDA 里的绝对地址要先减映像基址。

**例外**：有重载或模板实例的目标保持 **mangled**。符号库对它们只给 `_1` / `_2` / `_26`
这种纯序号（`MakeObject_10` 完全看不出是收 `Unit` 的那个），修饰名反而更可读、也更稳定。
各模块 README 的「目标」表里逐条标注了带不带 AOB、用哪种符号形式。

### AOB 兜底的唯一性

每条 AOB 都在当前 `GameCore_XP2_FinalRelease.dll` 的 `.text` 段内**唯一命中**
（逐条扫 PE 复核）。**同形函数太多的目标不带 AOB** —— 带上只会随机命中别的函数。

---

## 目录结构

```
recipes/GameCore/Civ6BasePlugin_GameCore/
├── README.md
└── dll/
    ├── Civ6BasePlugin_GameCore.vcxproj
    ├── Civ6BasePlugin_GameCore.vcxproj.filters
    ├── HookAPI.h
    ├── framework.h / pch.h / pch.cpp
    ├── dllmain.cpp                          # OnLoad / OnUnload
    ├── include/asmjit/                      # vendored asmjit（仅 UnitDamageCap 用）
    ├── lib/asmjit.lib
    ├── Common/GameCoreResolve.h             # 兼容性取址（符号名主路径 + AOB 兜底）
    ├── BugFix/
    │   ├── GreatWorkOptimizer/    + README.md
    │   ├── GreatWorkTypeLookup/   + README.md
    │   ├── GreatWorkYieldStack/   + README.md
    │   ├── GreatWorkPreserve/     + README.md
    │   ├── GreatWorkModifierAttach/ + README.md
    │   ├── InitUnitReturn/        + README.md
    │   └── UnitDamageCap/         + README.md
    └── APIExtension/
        ├── GreatWorkAPI/          + README.md
        ├── ModifierManagerAPI/    + README.md
        └── UnitLevelAPI/          + README.md
```

---

## 构建

```powershell
& 'E:\VisualStudio\Products\MSBuild\Current\Bin\amd64\MSBuild.exe' `
  'recipes\GameCore\Civ6BasePlugin_GameCore\dll\Civ6BasePlugin_GameCore.vcxproj' `
  /p:Configuration=Release /p:Platform=x64
```

产物：`x64/Release/Civ6BasePlugin_GameCore.dll`。

工程配置：`CIV6_PLUGIN_GAMECORE`、`ASMJIT_STATIC`、C++20、`/utf-8`、
**`/MT`（`MultiThreaded`）** —— 后者是硬性要求：vendored `asmjit.lib` 是
`MT_StaticRelease` 编译的，用默认的 `/MD` 会直接 `LNK2038` 报运行时库不匹配。

---

## 部署与验证

1. 把 `Civ6BasePlugin_GameCore.dll` 放进 `<Mod>/Binaries/Win64/Plugins/GameCore/`；
2. 启动游戏并**进入一局**（GameCore 插件只在进入对局时加载）；
3. 看 `Civ6DllLoader_Loader.log`，正常应出现：

```
Civ6BasePlugin_GameCore: OnLoad
[GreatWorkOptimizer] installed 2/2 hooks; buildingDefs=ok humanSkip=on
[GreatWorkTypeLookup] GreatWorkSlotType hooked; resolver=ok (modded types now resolvable)
[GreatWorkTypeLookup] GreatWorkObjectType hooked; resolver=ok (modded types now resolvable)
[GreatWorkYieldStack] hooked GetGreatWorkYieldChange; great work yields now stack
[GreatWorkPreserve] active: extra great work slot changes keep existing great works
[GreatWorkModifierAttach] active: great works from Great Person now fire GreatWorkAdded
[InitUnitReturn] active: InitUnitValidAdjacentHex now returns the unit instance
[UnitDamageCap] hooked SetDamage+0x27; Range upper bound now Unit::GetMaxDamage() (detour=NNN bytes, orig=0x...)
[GreatWorkAPI] symbols ok; GreatWorkManager.AddGreatWork/AddGreatWorkToCity registered
[ModifierManager] unit-method inject: reg=1 pushed=1 (unit:AttachModifierByID(id))
[UnitLevelAPI] experience-method inject: reg=1 pushed=1 (unit:GetExperience():SetLevel(n))
Civ6BasePlugin_GameCore: loaded
```

任一模块出现 `target not resolvable` 说明游戏更新后该目标漂移，
**该模块自动跳过、其余模块照常工作**，不会崩。

4. FireTuner 里验证 Lua 扩展：

```lua
-- 巨作
print(GreatWorkManager.AddGreatWork(0, 0))            -- playerID, greatWorkIndex
print(GreatWorkManager.AddGreatWorkToCity(0, 1, 2))   -- playerID, cityID, greatWorkIndex

-- 修饰符
print(ModifierManager ~= nil)
unit:AttachModifierByID("MODIFIER_ID")                -- 返回索引或 -1

-- 单位等级
unit:GetExperience():SetLevel(3)                      -- -> true
print(unit:GetExperience():GetLevel())                -- -> 3
print(unit:GetExperience():GetExperienceForNextLevel())  -- 阈值随等级变化
```

> 各模块的具体验证方法与日志样例见其 README。

---

## 注意事项

- **`/utf-8` 与 `/MT` 都是必需的**：前者因为仓库 `.editorconfig` 约定 UTF-8 无 BOM；
  后者因为 vendored `asmjit.lib` 是静态运行库编译的；
- **只支持 x64**；
- **GameCore 重载**：Loader 在 GameCore 卸载时会清掉落在该映像内的钩子与订阅；插件若在
  exe / 常驻模块域上注册过订阅，需要在下次 `OnLoad` 时重新注册；
- **`OnUnload` 应保持轻量**：钩子与脚本登记由 Loader 按 owner 统一回收，各模块的
  `*_Shutdown()` 只断引用，不要做复杂析构或释放已打补丁指向的内存；
- **asmjit 只在本工程用于 `UnitDamageCap`**（Global 侧另有一套用于 `UnlockModelLimit`）；
  **不需要 Zydis**（那是反汇编校验用的，本工程只生成代码）。

---

## 参考

- 仓库总览：[`README.md`](../../../README.md)
- 插件开发规范：[`docs/PLUGIN_GUIDE.md`](../../../docs/PLUGIN_GUIDE.md)
- Global 侧姊妹工程：[`recipes/Global/Civ6BasePlugin/README.md`](../../Global/Civ6BasePlugin/README.md)

[Civ6DllLoader]: https://github.com/sociometry/Civ6DllLoader