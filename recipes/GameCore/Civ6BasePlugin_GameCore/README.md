# Civ6BasePlugin_GameCore

Civilization VI 的 **GameCore 生命周期插件工程**，运行在 [Civ6DllLoader] runtime 的 **HookAPI** 插件架构之上。

- **插件类别**：GameCore 插件（预编译宏 `CIV6_PLUGIN_GAMECORE`），入口为 `OnLoad(HookContext* ctx)`
- **生命周期**：每次**进入对局**（Phase B）加载，**退出对局 / 返回主菜单 / 重载对局**时随整个 GameCore 映像一起卸载
- **部署位置**：`<Mod>/Binaries/Win64/Plugins/GameCore/Civ6BasePlugin_GameCore.dll`
- **构建**：MSBuild（Release | x64，C++20，`/utf-8`）

> **2026-10 修复**：上一版把 5 个模块"注释停用"了，原因是它们调用了已从 `HookAPI.h` 删除的
> `RegisterGameCoreReady` / `RegisterGameCoreTeardown` 与改名前的 `EditPlayer`。
> 现在 5 个模块**全部恢复参与编译**，并统一改成"符号名 + AOB 兜底 + 前导字节校验"的
> 兼容性取址（见 `dll/Common/GameCoreResolve.h`）。详见下方「兼容性修复」。

## 模块清单

| 模块 | 分类 | 功能 |
| --- | --- | --- |
| **GreatWorkOptimizer** | BugFix | `CultureAi_GreatWorks::Execute` 巨作分配优化：巨作集合未变时跳过 O(G³) 全量重算；`bounded_knapsack` 容量钳制（capacity > 3 与 3 等价）；人类玩家整次跳过 Execute |
| **GreatWorkTypeLookup** | BugFix | 巨作「名哈希 → 类型索引」解析修复：`NameToGreatWorkSlotType` / `NameToGreatWorkObjectType` 原把解析写死成 switch、未命中返回 -1，模组新增 `GreatWorkSlotType` / `GreatWorkObjectType` 后解析必败；改为先查数据驱动的定义集合解析器并维护哈希缓存表 |
| **GreatWorkYieldStack** | BugFix | 巨作产出加成不叠加修复：`City::Culture::GetGreatWorkYieldChange` 找到第一条匹配就立即返回，导致同一（物件类型，产出类型）多条来源只算一条；改为遍历整表把所有匹配条目的正数 amount 求和 |
| **GreatWorkAPI** | APIExtension | 巨作添加整合接口（Lua）：`GreatWorkManager.AddGreatWork(playerID, greatWorkIndex)`、`GreatWorkManager.AddGreatWorkToCity(playerID, cityID, greatWorkIndex)` |
| **ModifierManagerAPI** | APIExtension | 单位修饰符挂接与摘除：`unit:AttachModifierByID(modifierID)` 挂一层并返回数字索引，`ModifierManager.DetachModifierByID(unit, modifierID)` 精确摘除一层 |

> `ModConfig`（全局 Lua 配置接口）**不在本工程**：它由 Global 侧的 `RegisterGlobalScript`
> 注册 —— 那个入口的语义就是"注册全局可用的接口"，loader 会把它挂到每一个脚本环境
> （主菜单 / 对局 gameplay / 对局 UI）。详见 [`docs/MODCONFIG.md`](../../../docs/MODCONFIG.md)。

## 兼容性修复（本次）

原工程"停用模块"的三个根因与对应处理：

| 根因 | 处理 |
| --- | --- |
| 依赖已删除的 `RegisterGameCoreReady` / `RegisterGameCoreTeardown` | 直接删除代际回调：**GameCore 插件在 `OnLoad` 执行时 GameCore 早已加载完毕**，"等就绪"的语义由框架本身满足；退局时 Loader 按 owner 自动回滚钩子 |
| `GreatWorkAPI` 用了改名前的 `EditPlayer` | 改用 `HookAPI::GetPlayer` |
| 全部地址写死成裸 RVA + 前导字节哨兵 | 统一改为 `HookName(符号名)` 主路径 + 唯一 AOB 兜底 + `PrologueMatches` 校验（`dll/Common/GameCoreResolve.h`）。裸 RVA 在符号库与当前映像对不上时会被 Loader **原样透传**，等于把钩子装到错误的函数上 |

另外：

- `ModifierManagerAPI` 不再读硬编码的 `ms_pkCurrentGame` 全局地址，改调引擎自己的
  `Game::Instance::Edit()`（内部就是读那个全局），跨版本更稳；
- Lua 参数从 `LuaCheckInteger`（会抛 Lua 错误）改为 `LuaType` 判断 + `OptInteger`，
  非法参数返回 `false` / `-1`，不再让脚本报错；
- 人类玩家判定改用 `Player::Instance::IsHuman` 直调用（符号名唯一），解析失败时
  人类跳过分支自动失效，退回原版全量重算；
- `Execute` 的日志降为"首次 + 每 32 次一条"，避免 O(G³) 路径被日志拖慢。

### AOB 兜底的唯一性

每条 AOB 都在当前 `GameCore_XP2_FinalRelease.dll` 的 `.text` 段内**唯一命中**
（用 `tools/aob_scan.py` 直接扫 PE 复核）。**同形函数太多的目标不带 AOB**——
带上只会随机命中别的函数：

| 目标 | AOB | 说明 |
| --- | --- | --- |
| `CultureAi_GreatWorks::Execute` | 24 字节 | 唯一 |
| `bounded_knapsack` | 21 字节 | 唯一 |
| `GetBuildingDefinitions` | 18 字节（含 `??`） | 唯一 |
| `NameToGreatWorkSlotType` / `ObjectType` | 16 字节 | 唯一 |
| `GetGreatWorkYieldChange` | 16 字节 | 唯一 |
| `Culture::FindOrAddGreatWork` / `SetGreatWorkPlayer` | 21 / 18 字节 | 唯一 |
| `Buildings::CanAddGreatWork` / `AddGreatWork` / `AddGreatWorkToSlot` | 28 / 26 / 22 字节 | 唯一 |
| `Districts_Units_Cities::Find` | 20 字节 | 唯一 |
| `ModifierSystem::AttachModifier` / `ClearModifier` | 18 / 16 字节 | 唯一 |
| `Game::Instance::Edit` / `EditGameEffects` / `Handle::get` | 16 / 16 / 16 字节 | 唯一 |
| `Culture::Edit` | **不带 AOB** | 该前缀有 47 处同形命中，只用（唯一的）修饰名 |
| `Unit::GetInstance`（ScopedInstance 系列） | **不带 AOB** | 50 处同形命中 |
| `std::string::assign` | **不带 AOB** | 5 处同形命中 |
| `GameEffects::MakeObject<Unit::Instance>` | **不带 AOB** | 3 处同形命中 |
| `Player::Instance::IsHuman` | **不带 AOB** | 7 处同形命中 |

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
    ├── Common/GameCoreResolve.h             # 兼容性取址（符号 + AOB + 前导字节校验）
    ├── BugFix/
    │   ├── GreatWorkOptimizer/
    │   ├── GreatWorkTypeLookup/
    │   └── GreatWorkYieldStack/
    └── APIExtension/
        ├── GreatWorkAPI/
        └── ModifierManagerAPI/
```

> ModConfig 在 Global 工程：`recipes/Global/Civ6BasePlugin/dll/APIExtension/ModConfig/`。

## 构建

```powershell
& 'E:\VisualStudio\Products\MSBuild\Current\Bin\amd64\MSBuild.exe' `
  'recipes\GameCore\Civ6BasePlugin_GameCore\dll\Civ6BasePlugin_GameCore.vcxproj' `
  /p:Configuration=Release /p:Platform=x64
```

或直接跑仓库脚本：`bash tools/build_gamecore.sh`。

产物：`x64/Release/Civ6BasePlugin_GameCore.dll`（中间文件在 `dll/x64/$(Configuration)/`）。

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
[GreatWorkAPI] symbols ok; GreatWorkManager.AddGreatWork/AddGreatWorkToCity registered
[ModifierManager] unit-method inject: reg=1 pushed=1 (unit:AttachModifierByID(id))
Civ6BasePlugin_GameCore: loaded
```

任一模块出现 `target not resolvable` / `prologue mismatch` 说明游戏更新后该目标漂移，
**该模块自动跳过、其余模块照常工作**，不会崩。

4. FireTuner 里可验证本工程的两个 Lua 扩展：

```lua
print(GreatWorkManager.AddGreatWork(0, 0))        -- 巨作添加（playerID, greatWorkIndex）
print(GreatWorkManager.AddGreatWorkToCity(0, 1, 2)) -- 放进指定城市（playerID, cityID, greatWorkIndex）
print(ModifierManager ~= nil)                     -- 修饰符接口表存在
```

## 注意事项

- **`/utf-8` 是必需的**：仓库 `.editorconfig` 约定 UTF-8 无 BOM，MSVC 默认按系统代码页解析中文注释会报错；
- **只支持 x64**；
- **GameCore 重载**：Loader 在 GameCore 卸载时会清掉落在该映像内的钩子与订阅；
  插件若在 exe / 常驻模块域上注册过订阅，需要在下次 `OnLoad` 时重新注册；
- **本工程不引入 asmjit / Zydis / Zycore**（那三个只属于 Global 侧的 `UnlockModelLimit`）。

## 参考

- 仓库总览：`README.md`
- 插件开发规范：`docs/PLUGIN_GUIDE.md`
- Global 侧姊妹工程：`recipes/Global/Civ6BasePlugin/README.md`

[Civ6DllLoader]: https://github.com/sociometry/Civ6DllLoader
