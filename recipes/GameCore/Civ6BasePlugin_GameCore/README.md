# Civ6BasePlugin_GameCore

Civilization VI 的 **GameCore 生命周期插件工程**，跑在 [Civ6DllLoader] runtime 的 **HookAPI** 插件架构上。

- **插件类别**：GameCore（宏 `CIV6_PLUGIN_GAMECORE`），入口 `OnLoad(HookContext* ctx)`
- **生命周期**：每次**进入对局**加载，**退出对局 / 回主菜单 / 重载对局**时随整个 GameCore 映像一起卸载
- **部署位置**：`<Mod>/Binaries/Win64/Plugins/GameCore/Civ6BasePlugin_GameCore.dll`
- **构建**：MSBuild（Release | x64，C++20，`/utf-8`，`/MT`）

姊妹工程：[Global 侧](../../Global/Civ6BasePlugin/README.md)（常驻进程，含 ModConfig 与渲染上限解锁）。

---

## 模块清单

| 模块 | 分类 | 功能 |
| --- | --- | --- |
| **GreatWorkOptimizer** | BugFix | 巨作分配优化：巨作集合未变时跳过 O(G³) 全量重算；`bounded_knapsack` 容量钳制（capacity > 3 与 3 等价）；人类玩家整次跳过 |
| **GreatWorkTypeLookup** | BugFix | 巨作「名哈希 → 类型索引」解析修复：原实现写死 switch、未命中返回 -1，模组新增类型后必败；改为先查数据驱动定义集合 + 哈希缓存 |
| **GreatWorkYieldStack** | BugFix | 巨作产出加成不叠加修复：`GetGreatWorkYieldChange` 找到第一条匹配就返回，改为遍历整表求和 |
| **UnitDamageCap** | BugFix | **单位伤害上限对齐真实 `MaxHitPoints`**：MOD 把单位血量提到 100 以上时，原版伤害被全局常量钳住导致打不死 |
| **GreatWorkAPI** | APIExtension | 巨作添加整合接口（Lua）：`GreatWorkManager.AddGreatWork` / `AddGreatWorkToCity` |
| **ModifierManagerAPI** | APIExtension | 修饰符挂接/摘除：`unit:AttachModifierByID(id)`、`ModifierManager.DetachModifierByID` / `GetCount` |
| **UnitLevelAPI** | APIExtension | **单位等级写入**：`unit:GetExperience():SetLevel(n)`（引擎只有读，没有对外的写） |

> `ModConfig`（全局 Lua 配置接口）**不在本工程**：它由 Global 侧 `RegisterGlobalScript` 注册 ——
> 那个入口的语义就是"注册全局可用的接口"，loader 会挂到每一个脚本环境。详见
> [`docs/MODCONFIG.md`](../../../docs/MODCONFIG.md)。

---

## 1. UnitDamageCap —— 打不死的根因与修法

### 缺陷

XP2 的 `Unit::Instance::SetDamage`（RVA `0x3A5A60`）把伤害钳到**全局常量**
`Context::Globals::ms_pkParameters->COMBAT_MAX_HIT_POINTS`（默认 100）：

```asm
13f3c5a87  44 8B 80 E8 01 00 00   mov r8d, [rax+1E8h]   ; COMBAT_MAX_HIT_POINTS
13f3c5a8e                        call Utilities::Range  ; Range(damage, 0, 上面那个)
```

Base 版（`0x18029CC40`）在同一位置多一步：按 hash `0x7D3FC711` 从
`unit->m_kProperties(+0x1F0)` 取 **`MaxHitPoints` 属性**当上限，取不到才回退全局常量。
XP2 把这段删了。

**后果**：MOD 把某单位 `MaxHitPoints` 调到 100 以上时，`SetDamage` 最多写入 100，
永远到不了 `MaxHitPoints`，单位不再死亡。`ChangeDamage(+n)` 只是
`SetDamage(m_iDamage + n)` 的薄封装，改 `SetDamage` 一处即可覆盖所有加伤路径。

### 修法：只替换一条指令

不复刻整个函数，也不"调原函数再补写"（那会写两次、事件发两次、绕过脏标记）。做法是把
**取上限的那一条指令**改成跳板：

```asm
; 原：44 8B 80 E8 01 00 00      mov r8d, [rax+1E8h]
; 新：E9 xx xx xx xx 90 90      jmp trampoline; nop; nop
```

trampoline（asmjit 生成）里按单位算出真实上限写进 `r8d`，再跳回 `call Utilities::Range`。

这样得到：

- `SetDamage` **只执行一次、只赋值一次**；
- `UNIT_DAMAGE_CHANGED` 只发一次，且数值就是最终值；
- `MoveAssignmentDo` 照常走，**FAutoVariable 的脏标记机制完整保留**（联机同步不受影响）；
- 除这一条指令外，函数的 Heal 取消等逻辑一个字都不动。

### asmjit 的两个坑（实测）

| 写法 | 实际生成 | 说明 |
| --- | --- | --- |
| `a.jmp((uint64_t)addr)` | `FF 25 02 00 00 00` + 内联指针（16 字节） | **间接跳转**——读目标处 8 字节当指针再跳，不是"跳到该地址"，用它必崩 |
| `a.jmp(x86::ptr(addr))` | `RelocOffsetOutOfRange` | 报错，不可用 |
| `a.jmp(Label)` | `E9 xx xx xx xx` | 能生成 `E9 rel32`，但 Label 只能指向同一 CodeHolder 内部 |
| `a.embedUInt8(0xE9); a.embedInt32(rel)` | `E9 ...` | **本工程用这个**：asmjit 负责编码，目标地址延后回填 |

trampoline 地址是运行期分配的，所以 rel32 只能占位后回填。

另外 **asmjit 对非法操作数只写进 emitter 的局部 `err`**，`rt.add()` 仍返回 `kErrorOk`，
那条指令会被静默丢弃 —— 所以每条 emit 都必须自己检查（`UC_EMIT` / `Emit` 宏）。

### 定位与依赖

| 目标 | RVA | 说明 |
| --- | --- | --- |
| `Unit::Instance::SetDamage` | `0x3A5A60` | 补丁点 `+0x27`，回跳点 `+0x2E` |
| `Unit::Instance::GetMaxDamage` | `0x3A39A0` | 就是 XP2 里"读 MaxHitPoints 属性，取不到回退全局"那段 |

补丁前会 `memcmp` 比对原指令字节（`44 8B 80 E8 01 00 00`），不符即跳过，不会打错位置。

> `Shutdown` 是**空实现**：退局时插件整体卸载、模块内存由引擎回收，主动 `VirtualFree`
> trampoline 反而危险 —— 补丁字节还指向它。按 `docs/PLUGIN_GUIDE.md` §6.3，补丁本就
> "按生效后不还原"设计。

---

## 2. UnitLevelAPI —— 给单位补 SetLevel

### 为什么需要

引擎的 Lua 接口把单位经验相关方法都挂在 `Unit::Experience` 表下：
`GetLevel` / `GetExperiencePoints` / `GetExperienceForNextLevel` / `ChangeExperience` /
`SetPromotion` / `SetVeteranName`……**唯独没有写等级的方法**。

引擎内部倒是有 `Experience::SetLevel(int)`（RVA `0x3974A0`），但没暴露到 Lua。

### 做法

照搬引擎自己的挂载方式（`ScopedInstance<IUnitExperience,Unit::Experience>::Push`）：

```cpp
RegisterPushMethodsTarget("GameCore::Lua::IUnitExperience", kHookApiPushNoManager,
    "GameCore::Lua::IUnitExperience::PushMethods");
PushLuaMethods("GameCore::Lua::IUnitExperience", &s_setLevel, 1);
```

调用形式与同族接口一致：

```lua
unit:GetExperience():SetLevel(3)   -- -> boolean
```

### 只包一层，不改语义

`SetLevel` 只写等级字段（`+0x10`）并发一个 `UNIT_EXPERIENCE_CHANGED` 事件，
**不动累计经验**（`+0x0C`）——这是引擎原本的行为，本模块保持原样。

关于"升级所需经验"：阈值不是存储字段，而是由等级实时算出
（`3 * MULT * ((level + level²) / 2)`，入参就是 `+0x10`），所以等级一写，阈值立刻就是
新等级的；UI/Lua 读的是 Cache 层的缓存副本，靠 `0xA038D885` 事件驱动重算，而这个事件
`SetLevel` 自己就会发。**因此本模块不插手经验值，也不补发事件。**

### 依赖面刻意压到最小

只解析 **2 个**官方函数：

| 用途 | 符号 | 方式 |
| --- | --- | --- |
| 写等级 | `GameCore::Unit::Experience::SetLevel` | `HookName`（demangled）+ AOB 兜底 |
| 取实例 | `GameCore::Lua::ScopedInstance<GameCore::Lua::IUnitExperience,GameCore::Unit::Experience>::GetInstance` | `HookName`（demangled）+ AOB 兜底 |

少取一个就少一处"可能解析失败"。

---

## 兼容性取址

所有目标统一走 `HookName(符号名)` 主路径 + 唯一 AOB 兜底 + `PrologueMatches` 校验
（见 `dll/Common/GameCoreResolve.h`）。裸 RVA 在符号库与当前映像对不上时会被 Loader
**原样透传**，等于把钩子装到错误的函数上。

符号名一律用 **demangled** 形式（`tools/rva_to_symbol.exe` 反查得到），可读性最好：

```bash
tools\rva_to_symbol.exe -gamecore 0x3A5A60 --demangled
```

> 输入是 **RVA 不是 VA**：IDA 里的绝对地址要先减映像基址。

**例外**：有重载的目标保持 mangled。符号库对重载只给 `_1` / `_2` / `_10` 这种纯序号
（例如 `GameEffects::MakeObject_10` 完全看不出是收 `Unit` 的那个），mangled 反而更可读、
也更稳定。详见下表"不带 AOB"的几项。

### AOB 兜底的唯一性

每条 AOB 都在当前 `GameCore_XP2_FinalRelease.dll` 的 `.text` 段内**唯一命中**
（`tools/aob_scan.py` 直接扫 PE 复核，当前 23 条全唯一）。**同形函数太多的目标不带 AOB** ——
带上只会随机命中别的函数：

| 目标 | AOB | 说明 |
| --- | --- | --- |
| `Unit::Instance::SetDamage` | 28 字节（含 `??`） | 唯一；前缀本身有 5 处同形，延伸后才唯一 |
| `Unit::Instance::GetMaxDamage` | 16 字节 | 唯一 |
| `Unit::Experience::SetLevel` | 15 字节 | 唯一 |
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
| `Culture::Edit` | **不带 AOB** | 前缀 47 处同形 |
| `ScopedInstance<...>::GetInstance` 系列 | **不带 AOB** | 50 处同形 |
| `std::string::assign` | **不带 AOB** | 5 处同形 |
| `GameEffects::MakeObject`（4 个重载） | **不带 AOB** | 3 处同形 + 14 个重载 |
| `Player::Instance::IsHuman` | **不带 AOB** | 7 处同形 |

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
    ├── Common/GameCoreResolve.h             # 兼容性取址（符号 + AOB + 前导字节校验）
    ├── BugFix/
    │   ├── GreatWorkOptimizer/
    │   ├── GreatWorkTypeLookup/
    │   ├── GreatWorkYieldStack/
    │   └── UnitDamageCap/
    └── APIExtension/
        ├── GreatWorkAPI/
        ├── ModifierManagerAPI/
        └── UnitLevelAPI/
```

> ModConfig 在 Global 工程：`recipes/Global/Civ6BasePlugin/dll/APIExtension/ModConfig/`。

---

## 构建

```powershell
& 'E:\VisualStudio\Products\MSBuild\Current\Bin\amd64\MSBuild.exe' `
  'recipes\GameCore\Civ6BasePlugin_GameCore\dll\Civ6BasePlugin_GameCore.vcxproj' `
  /p:Configuration=Release /p:Platform=x64
```

或直接跑仓库脚本：`bash tools/build_gamecore.sh`。

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
[UnitDamageCap] patched SetDamage clamp bound -> Unit::GetMaxDamage()
[GreatWorkAPI] symbols ok; GreatWorkManager.AddGreatWork/AddGreatWorkToCity registered
[ModifierManager] unit-method inject: reg=1 pushed=1 (unit:AttachModifierByID(id))
[UnitLevelAPI] experience-method inject: reg=1 pushed=1 (unit:GetExperience():SetLevel(n))
Civ6BasePlugin_GameCore: loaded
```

任一模块出现 `target not resolvable` / `prologue mismatch` 说明游戏更新后该目标漂移，
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
- ModConfig 详解：[`docs/MODCONFIG.md`](../../../docs/MODCONFIG.md)
- Global 侧姊妹工程：[`recipes/Global/Civ6BasePlugin/README.md`](../../Global/Civ6BasePlugin/README.md)

[Civ6DllLoader]: https://github.com/sociometry/Civ6DllLoader