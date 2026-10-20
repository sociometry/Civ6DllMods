# GreatWorkAPI

> 分类：**APIExtension** · 返回 [工程总览](../../../README.md)

给模组作者补上「添加巨作」的 Lua 接口。

## 为什么需要

原版 Lua **没有任何**往城市添加巨作的接口。模组想送玩家一件巨作，只能靠
触发伟人、伪造事件之类的迂回手段，既不可靠也无法指定城市 / 槽位。

引擎内部倒是有完整的一套函数，但不对外暴露。

## 接口

```lua
-- 自动选城：遍历玩家的城市，放进第一个放得下的
GreatWorkManager.AddGreatWork(playerID, greatWorkIndex)          --> boolean

-- 指定城市
GreatWorkManager.AddGreatWorkToCity(playerID, cityID, gwIndex)  --> boolean
```

| 参数 | 含义 |
| --- | --- |
| `playerID` | 玩家 ID |
| `cityID` | 城市 ID（`AddGreatWorkToCity` 用） |
| `greatWorkIndex` | **数据库 Index**（`GameInfo.GreatWorks` 的 `.Index`），不是 `GreatWorkListIndex` |

非法参数（负数、类型不对）返回 `false`，**不抛 Lua 错误**。

```lua
local gw = GameInfo.GreatWorks.GREATWORK_MICHELANGELO_1.Index
print(GreatWorkManager.AddGreatWorkToCity(playerId, cityId, gw))  --> true
```

## 内部调用链

```
Game::Culture::Edit()                        -> Culture&
Culture::FindOrAddGreatWork(culture, type)   -> listIndex
Culture::SetGreatWorkPlayer(culture, idx, playerId)
Buildings::CanAddGreatWork(buildings, idx, nullptr)  -> 能否放下
Buildings::AddGreatWork(buildings, idx)              -> 真正放入（自动挑合法槽）
```

### 为什么用 `AddGreatWork` 而不是 `AddGreatWorkToSlot(..., -1)`

这两个函数语义不同：

| 函数 | 行为 |
| --- | --- |
| `AddGreatWork` | 逐槽调用 `IsValidObjectForSlot` 校验**槽类别 vs 巨作类别**，只放进合法空槽 |
| `AddGreatWorkToSlot(idx, bldType, -1)` | 在该建筑记录里找**第一个空槽**直接写，**不校验类别** |

后者会把著作塞进艺术槽之类。因此放置统一走 `AddGreatWork`。

## 同种巨作多份的支持

### 问题

`Culture::FindOrAddGreatWork` 按 `greatWorkType` **查找**，同类型永远返回同一个 `listIndex`。

于是「多份同种巨作」= 同一个 index 出现在多个槽里。而引擎所有槽操作都假设
「一个 index 只在一个槽」：

```c
RemoveGreatWork(index)  // 扫到第一个匹配就停，只清那一个，返回该槽下标
```

结果就是**移动巨作时清错槽** —— 清掉的是另一个槽里的同 index 巨作。
CE 的同样实现也有这个问题。

### 解法：检测占用 → 造新条目

```
1. 正常调 FindOrAddGreatWork(type) 拿到 index i
2. 若 i 已经躺在该 Buildings 的某个槽里（说明是第 2、3… 份）：
     a. 用一个列表里不存在的【假类型】调 FindOrAddGreatWork
        —— 查找必然未命中，引擎走自己的 DoInsertValue 追加路径
     b. 重新取新条目地址（可能因扩容而移动），把类型改成目标类型
        返回新下标 j
3. SetGreatWorkPlayer(j, playerId)
```

这样完全复用引擎的追加逻辑（内存管理、字段初始化、容量增长全部正确），
不需要手工构造 `GreatWork`、不需要调 `DoInsertValue`、不需要 hook 任何函数。

`IndexInUse` 只扫描**这一个 `Buildings` 对象**的槽 —— 因为 `RemoveGreatWork` 也只在
单个 `Buildings` 内扫描，所以「同一建筑内 index 唯一」就足以让引擎行为正确。

### ⚠️ 两条走不通的路（会导致崩溃，勿试）

**1. 把既有条目的 `greatWorkType` 临时改成 `-1`，迫使查找未命中**

```
崩溃于 CultureAi::DoOffTurn:
  mov rax, [rdx+rdi*8]   ; rdi = 0xFFFFFFFF
  mov rax, [rax+0B8h]    ; rax = 0 -> 访问 0xB8 -> ACCESS_VIOLATION
```

列表里存在 `type == -1` 的条目，AI 拿它去查 `Definition::GreatWork` 得到 `nullptr`，
随后直接解引用 `+0xB8`。而且这个 `-1` 会**持久化进存档**，读档后继续崩。

**2. 临时把 vector 的 `end` 指针前移一格，让查找看不到最后一条**

`DoInsertValue` 收到的 `pos` 正好等于（前移后的）`end`，会走「就地构造」路径，
**直接覆盖掉原最后一条**，数据丢失。

## 内存布局（汇编核实）

```
Player::Instance +0x6D0 = Player::Cities
Player::Cities   +0xD0  = 首都城市链表头；node[0]=City*，node[2]=next
City::Instance   +0xCB0 = Buildings 对象本体（+0xCA0 是它的 FAutoVariable 包装器）

Culture +0x118 = greatWorks vector.begin
        +0x120 = vector.end
        +0x128 = vector.capacity
        （元素 24 字节：+0 greatWorkType，+4 player，+8 turn，+16 name）
```

## 目标

| 目标 | 方式 | AOB |
| --- | --- | --- |
| `Game::Culture::Edit` | `HookName` | **不带**（47 处同形） |
| `Culture::FindOrAddGreatWork` | `HookName` + AOB | 21 字节，唯一 |
| `Culture::SetGreatWorkPlayer` | `HookName` + AOB | 18 字节，唯一 |
| `Buildings::CanAddGreatWork` | `HookName` + AOB | 28 字节，唯一 |
| `Buildings::AddGreatWork` | `HookName` + AOB | 26 字节，唯一 |
| `Buildings::AddGreatWorkToSlot` | `HookName` + AOB | 22 字节，唯一（仅保留解析与就绪判定，不用于放置） |
| `Player::Districts_Units_Cities::Find` | `HookName` + AOB | 20 字节，唯一 |

## 注册环境

只注册到 **gameplay** 脚本环境 —— 这两个接口操作的是引擎 gameplay 域对象
（`Culture` / `Buildings`），UI 脚本环境用不到，也不该在那里暴露。

## 日志

```
[GreatWorkAPI] symbols ok; GreatWorkManager.AddGreatWork/AddGreatWorkToCity registered
[GreatWorkAPI] AddGreatWorkToCity ok: player=0 city=65536 gw=0 list=0
[GreatWorkAPI] duplicate great work: gw=0 reuse=0 -> new=5
```

`duplicate great work` 出现说明触发了多份支持：`reuse` 是原本会复用的 index，
`new` 是新分配的唯一 index。
