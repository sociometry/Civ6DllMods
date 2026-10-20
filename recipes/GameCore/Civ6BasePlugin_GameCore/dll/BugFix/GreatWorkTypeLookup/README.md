# GreatWorkTypeLookup

> 分类：**BugFix** · 返回 [工程总览](../../../README.md)

让模组新增的巨作类型与巨作槽类型能被引擎识别。

## 问题

引擎把「名称 → 类型索引」的转换写成了**两张硬编码 switch 表**：

- `Definition::Utility::NameToGreatWorkSlotType` —— 名称哈希 → 槽类型
- `Definition::Utility::NameToGreatWorkObjectType` —— 名称哈希 → 物件类型

两张表的 `default` 分支都直接返回 `-1`。于是：

| 现象 | 根因 |
| --- | --- |
| 模组新增的巨作类型（如自定义的「雕塑」）放不进任何槽 | ObjectType 查不到 → `IsValidObjectForSlot` 恒 false |
| 模组新增的槽类型（如自定义的「陈列柜」）建不出来 | SlotType 返回 -1 → `AddGreatWorkSlots` 跳过该定义 |

## 做法

引擎其实**还有一条数据驱动路径**：`Types::GetIndex(类型)` —— 它在定义集合里按哈希查找，
命中就返回索引。这条路径天生支持模组内容。

所以修法是：**hook 那两个硬编码 switch，在返回 -1 时改走数据驱动解析器**。

```
原版：  名称哈希 → [硬编码 switch] → 索引 或 -1
本模块：名称哈希 → [硬编码 switch] → 索引
                              ↓ (-1 时)
                        [Types::GetIndex] → 索引 或 -1
```

## 反编译核实的硬编码表

**槽类型**（`NameToGreatWorkSlotType`）：

| 名称 | 哈希 |
| --- | --- |
| MUSIC | `0x2F6C7BC9` |
| RELIC | `0x8ABFC848` |
| PALACE | `0xC8609506` |
| HERO | `0xD8878949` |
| WRITING | `0x00B22BE8` |
| PRODUCT | `0x3E60D2DE` |
| ARTIFACT | `0x6868A539` |
| ART | `0x699389CB` |
| CATHEDRAL | `0x6DB6854C` |

**物件类型**（`NameToGreatWorkObjectType`）：

| 名称 | 哈希 |
| --- | --- |
| SCULPTURE | `0x05AFB7A8` |
| PRODUCT | `0x8869D47F` |
| RELIGIOUS | `0xACA72322` |
| MUSIC | `0xB487A51F` |
| WRITING | `0xB6BB2D49` |
| ARTIFACT | `0xC90F3F41` |
| RELIC | `0x1154169E` |
| PORTRAIT | `0x14AA6B96` |
| LANDSCAPE | `0x1E0E0BF4` |
| HERO | `0x579BEF89` |

这两张表本身**不改动** —— 只在它们返回 `-1` 时才接管，所以原版内容的解析路径完全不变。

## 受益面

四个调用面：

| 函数 | 影响 |
| --- | --- |
| `Buildings::AddGreatWorkSlots` | 新增槽类型能否被建出来 |
| `Culture::IsValidObjectForSlot` | 新增巨作能否放进槽 |
| `AI::GreatWorkBuildingInfo` / AI ValueManager | AI 能否正确估值 |
| `Effects::AdjustExtraGreatWorkSlots::Initialize` | 改槽数效果能否作用于新槽 |

物件类型侧还额外覆盖 `Culture::Reset`、`Religion::GetNearestRelicSlot`、
`Congress ResolutionGreatWorkObjectTarget` 族、`AdjustGreatWorkObject*` 效果等。

## 目标

| 目标 | 符号形式 | 说明 |
| --- | --- | --- |
| `NameToGreatWorkSlotType` | `HookName`（demangled）+ AOB | 16 字节，唯一 |
| `NameToGreatWorkObjectType` | `HookName`（demangled）+ AOB | 16 字节，唯一 |
| `Types::GetIndex<GreatWorkSlotTypes>` | **MSVC 修饰名** | demangled 名带 `_26` 序号，跨版本会变 |
| `Types::GetIndex<GreatWorkObjectTypes>` | **MSVC 修饰名** | 同上 |

解析器不用 demangled 名，是因为符号库对模板实例只给 `Types::GetIndex_26` 这种纯序号，
序号会随无关代码增删而漂移；修饰名反而是稳定的全名。

## 缓存

解析结果放进带**容量上限**的缓存表（上限 4096），避免异常哈希把表撑爆。

## 来源

槽类型部分从 CivilizationVI_CommunityExtension 1.3.0-beta 的 `Main.cpp`
（`PatternTest` + `Hook_S`）迁移；物件类型部分按同一缺陷模式扩展（CE 未覆盖）。

## 日志

```
[GreatWorkTypeLookup] GreatWorkSlotType hooked; resolver=ok (modded types now resolvable)
[GreatWorkTypeLookup] GreatWorkObjectType hooked; resolver=ok (modded types now resolvable)
```

解析器不可用时降级为「只用硬编码表」，原版行为不变：

```
[GreatWorkTypeLookup] %s: data-driven resolver unavailable - falling back to the hardcoded table only
```
