# UnitLevelAPI

> 分类：**APIExtension** · 返回 [工程总览](../../../README.md)

给单位补上 `SetLevel`（引擎只有读，没有对外的写）。

## 为什么需要

引擎的 Lua 接口把单位经验相关方法都挂在 `Unit::Experience` 表下：

`GetLevel` / `GetExperiencePoints` / `GetExperienceForNextLevel` / `ChangeExperience` /
`SetPromotion` / `SetVeteranName` …… **唯独没有写等级的方法**。

引擎内部倒是有 `Experience::SetLevel(int)`（RVA `0x3974A0`），但没暴露到 Lua。

## 接口

```lua
unit:GetExperience():SetLevel(3)   --> boolean
```

调用形式与同族接口完全一致（挂在 `Unit::Experience` 上，不是 `Unit` 上）。

## 做法：照搬引擎自己的挂载方式

```cpp
RegisterPushMethodsTarget("GameCore::Lua::IUnitExperience", kHookApiPushNoManager,
    "GameCore::Lua::IUnitExperience::PushMethods");
PushLuaMethods("GameCore::Lua::IUnitExperience", &s_setLevel, 1);
```

## 只包一层，不改语义

`SetLevel` 只写等级字段（`+0x10`）并发一个 `UNIT_EXPERIENCE_CHANGED` 事件，
**不动累计经验**（`+0x0C`）—— 这是引擎原本的行为，本模块保持原样。

反汇编：

```asm
13f3b74a0  sub rsp, 38h
13f3b74a4  mov rax, [rcx]              ; m_pUnit
13f3b74aa  mov [rcx+10h], edx          ; m_iLevel = level
13f3b74b2  mov [rsp+..], 0A038D885h    ; UNIT_EXPERIENCE_CHANGED
13f3b74e7  call Event::Send
13f3b74f0  retn
```

### 为什么不用管「升级所需经验」

阈值不是存储字段，而是由等级**实时算出**：

```
3 * MULT * ((level + level²) / 2)     // 入参就是 +0x10
```

所以等级一写，阈值立刻就是新等级的。

UI / Lua 读的是 Cache 层的缓存副本，靠 `0xA038D885` 事件驱动重算 —— 而这个事件
`SetLevel` 自己就会发。**因此本模块不插手经验值，也不补发事件。**

## 依赖面刻意压到最小

只解析 **2 个**官方函数：

| 用途 | 符号 | 方式 |
| --- | --- | --- |
| 写等级 | `GameCore::Unit::Experience::SetLevel` | `HookName`（demangled）+ AOB |
| 取实例 | `GameCore::Lua::ScopedInstance<GameCore::Lua::IUnitExperience,GameCore::Unit::Experience>::GetInstance` | `HookName`（demangled）+ AOB |

少取一个就少一处「可能解析失败」。

## 目标

| 目标 | RVA | AOB |
| --- | --- | --- |
| `Experience::SetLevel` | `0x3974A0` | 16 字节，唯一 |
| `Lua::ScopedInstance<IUnitExperience>::GetInstance` | `0x6FFED0` | **不带**（只靠符号名解析） |

> `GetInstance` 是模板实例，`UnitLevelAPI.cpp` 里只写了 `HookName(...)` 一个参数，没有 AOB 兜底。

## 日志

```
[UnitLevelAPI] experience-method inject: reg=1 pushed=1 (unit:GetExperience():SetLevel(n))
```
