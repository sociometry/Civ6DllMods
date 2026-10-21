# ModifierManagerAPI

> 分类：**APIExtension** · 返回 [工程总览](../../../README.md)

给模组作者补上「挂载 / 摘除修饰符」的 Lua 接口。

## 为什么需要

原版 Lua 可以给单位挂修饰符，但：

- **摘不掉** —— 没有对应的 Detach 接口；
- **数不清** —— 不知道某个 ID 当前挂了几层；
- **限定身份** —— 只有单位能用，城市 / 玩家 / 区域都没有。

## 接口

```lua
-- 挂载（实例方法，注入到 IUnit）
unit:AttachModifierByID(modifierID)                 --> number   -- 返回索引，失败 -1

-- 摘除（owner 可为 unit / city / player / district）
ModifierManager.DetachModifierByID(owner, id[, n])  --> number   -- 返回实际摘掉的层数（0 = 没找到）；n 缺省 = 摘一层

-- 计数
ModifierManager.GetCount(owner, id)                 --> number
```

`n` 超过实际层数时全摘。重复摘除返回 `0` 而不是报错（注意返回值是**层数**不是布尔）。

```lua
local idx = unit:AttachModifierByID("COMMANDO_BONUS_MOVEMENT")
print(ModifierManager.GetCount(unit, "COMMANDO_BONUS_MOVEMENT"))  -- 1
ModifierManager.DetachModifierByID(unit, "COMMANDO_BONUS_MOVEMENT", 1)
print(ModifierManager.GetCount(unit, "COMMANDO_BONUS_MOVEMENT"))  -- 0
```

## 核心设计：Detach 不依赖自己的台账

这是本模块最关键的决定。**不去记录「我挂过什么」，而是直接问引擎当前有什么**：

```
MakeObject(owner)                    -> shared_ptr<IModifierObject>
GetModifiersOwnedBy(system, outVec, sp)  -> 自有索引列表
逐个比 ID -> ClearModifier(system, index) -> IModifier::Deactivate
```

好处是：

| 场景 | 能否正确摘除 |
| --- | --- |
| 引擎自己挂的层 | ✅ |
| 别的插件挂的层 | ✅ |
| 跨存档遗留的层 | ✅ |
| 用其他路径（效果 / 事件）挂的层 | ✅ |

**与谁挂的无关** —— 只要当前存在于引擎的修饰符系统里，就能被找到并摘掉。

## 踩过的坑：只 ClearModifier 不 Deactivate

`ClearModifier` 只把索引从 map 里移除，**不会撤销属性加成**。

实测现象：挂上 `+1 移动力` 后摘除，`GetMaxMoves()` 仍停在 5。

引擎自己的每条 Remove 路径都是 **先 `IModifier::Deactivate` 再 `ClearModifier`**，
本模块照此执行，问题消失。

## 四种 owner 的重载

每类 owner 各有一份 `MakeObject` 与 `GetInstance`：

| owner | MakeObject | GetInstance |
| --- | --- | --- |
| Unit | 独立重载 | `ScopedInstance<IUnit,...>::GetInstance` |
| City | 独立重载 | `ScopedVirtualInstance<ICity,...>::GetInstance` |
| Player | 独立重载 | `IPlayer::GetPlayerInstance`（**不是**模板实例） |
| District | 独立重载 | `ScopedInstance<IDistrict,...>::GetInstance` |

> Player 不能用 `ScopedVirtualInstance<IPlayer,PlayerReference>`：它只返回 `PlayerReference`（ID 引用），
> 拿不到 `Player::Instance*`，与 `MakeObject(Player)` 的入参对不上。

符号库对重载只给 `_1` / `_2` / `_10` 这种纯序号（`MakeObject_10` 完全看不出是收
`Unit` 的那个），所以**有重载的目标一律保持 mangled**，反而更可读也更稳定。

## 目标

| 目标 | 方式 | AOB |
| --- | --- | --- |
| `ModifierSystem::AttachModifier` | `HookName` + AOB | 18 字节，唯一 |
| `ModifierSystem::ClearModifier` | `HookName` + AOB | 19 字节，唯一 |
| `GetModifiersOwnedBy` | `HookName` + AOB | 17 字节，唯一 |
| `GetModifierOwnedByCount` | `HookName` + AOB | 16 字节，唯一 |
| `Game::Instance::Edit` | `HookName` + AOB | 19 字节，唯一 |
| `Game::Instance::EditGameEffects` | `HookName` + AOB | 16 字节，唯一 |
| `Handle::get` | `HookName` + AOB | 23 字节，唯一 |
| `ScopedInstance<*>::GetInstance`（Unit/District） | `HookName` | **不带**（模板实例前缀不唯一） |
| `ScopedVirtualInstance<ICity>::GetInstance` | `HookName` | **不带**（同上） |
| `IPlayer::GetPlayerInstance` | `HookName` | **不带**（前缀不唯一，且不是模板实例） |
| `GameEffects::MakeObject`（4 个重载） | **mangled** `HookName` | Unit/City/Player **不带**（三者头部逐字节相同、命中 3 处）；**District 带 16 字节 AOB**（唯一命中） |
| `std::string::assign` | `HookName` | **不带**（前缀不唯一） |

> 命中数以 `ModifierManagerAPI.cpp` 顶部注释记录的实测值为准（ScopedInstance 系 **731** 处、
> `IPlayer::GetPlayerInstance` **109** 处、`std::string::assign` **82** 处、`MakeObject(Unit/City/Player)` **3** 处）。
> AOB 字节数以同文件 `HookName(sym, "...")` 的实际串为准；改代码时同步本表，不要凭记忆填。

## 挂载方式

`unit:AttachModifierByID` 是**实例方法**，通过 `PushLuaMethods` 注入到
`GameCore::Lua::IUnit` 的 PushMethods 里：

```cpp
PushLuaMethods("GameCore::Lua::IUnit::PushMethods", kHookApiPushNoManager, &s_attachModifier, 1);
```

## 来源

从 CivilizationVI_CommunityExtension 1.3.0-beta 的 `Unit.cpp`
（`lAttachModifierByID` / `lDetachModifierByID`）迁移到 Civ6DllLoader 插件框架。

## 日志

```
[ModifierManager] unit-method inject: reg=1 pushed=1 (unit:AttachModifierByID(id))
[ModifierManager] symbols resolved; unit:AttachModifierByID available
[ModifierManager] attach ok: unit=000000023AD9BE18 id=COMMANDO_BONUS_MOVEMENT idx=823
[ModifierManager] detach ok: id=COMMANDO_BONUS_MOVEMENT idx=823
[ModifierManager] attach failed: unit=... id=MODIFIER_DOES_NOT_EXIST_XYZ status=1
```
