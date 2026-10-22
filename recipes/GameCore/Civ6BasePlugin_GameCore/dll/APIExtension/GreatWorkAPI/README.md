# GreatWorkAPI

> 中文 | [English](README.en.md)

> 分类：**APIExtension** · 返回 [工程总览](../../../README.md)

给模组作者补上「添加 / 移除巨作」的 Lua 接口。

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

-- 移除：玩家范围内第一件该类型的巨作（同类型有多份时只删一件）
GreatWorkManager.RemoveGreatWork(playerID, greatWorkType)       --> boolean
```

| 参数 | 含义 |
| --- | --- |
| `playerID` | 玩家 ID |
| `cityID` | 城市 ID（`AddGreatWorkToCity` 用） |
| `greatWorkIndex` / `greatWorkType` | **数据库 Index**（`GameInfo.GreatWorks` 的 `.Index`），不是 `GreatWorkListIndex`。两个参数名是同一个东西，按语义分别命名 |

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
GreatWorkTypeOfListIndex(culture, idx)               -> 回查 greatWorkType
GreatWorkModifierAttach_NotifyAdded(buildings, type) -> 通知 GameEffects 挂 Modifier
```

### 最后一步是干什么的

巨作放进去之后还要通知 GameEffects 侧一次，否则挂在它身上的 Modifier 不会生效。
这一步由 [GreatWorkModifierAttach](../../BugFix/GreatWorkModifierAttach/README.md)
提供（它同时修好了原版伟人激活的同款缺陷）。

注意 `AddGreatWork` 收的是 `greatWorkListIndex`，而信号要的是 `greatWorkType` ——
同种巨作的第 2、3 份是不同 listIndex、同一 type，所以必须回查 Culture 列表翻译：

```c
int GreatWorkTypeOfListIndex(void* culture, int listIndex) {
    char* entry = GetGreatWorkEntry(culture, listIndex);   // listIndex * 0x18
    return entry ? *(int*)(entry + OFF_GW_TYPE) : -1;      // OFF_GW_TYPE = 0x00
}
```

模块未加载时该通知是空实现，接口主流程不受影响。

## 移除：按「巨作类型」找第一件

引擎**没有**「按 greatWorkType 移除」的函数。唯一的移除入口是：

```c
int Buildings::RemoveGreatWork(Buildings*, GreatWorkListIndex listIndex);
// 扫所有建筑的槽，命中第一个 greatWorkIndex == listIndex 的槽，写 -1 并返回槽下标
```

它收的是 **listIndex**（Culture 列表下标），而接口给的是 **greatWorkType**。
两者不能混用：同种巨作的第 2、3… 份各占一条列表条目，listIndex 不同、type 相同。
所以移除分两步走：

```
1. 遍历该玩家的每座城市 -> 每个建筑的每个槽
     槽里只有 listIndex -> 回查 Culture 列表得到 type -> 比对目标 greatWorkType
     命中即停，得到 listIndex
2. Buildings::RemoveGreatWork(buildings, listIndex)   -> 清掉那个槽
3. ClearGreatWorkOwner(culture, listIndex)            -> 条目 player 字段写回 -1
4. GreatWorkModifierAttach_NotifyRemoved(buildings, greatWorkType)
```

遍历顺序 = 城市链表（首都优先）-> 建筑记录 -> 槽位，与引擎自己的扫描顺序一致，
所以「同类型多份只删第一件」的行为是可预期的。

### 为什么条目只清 owner、不删条目

`greatWorks` 的 listIndex 是**存档持久化**的下标。从 vector 里删元素会让后面所有
条目前移，旧存档和其它模块手上缓存的 index 立刻错位。所以条目原样保留，只把
`player` 写回 `-1`（引擎自己的「未分配」哨兵）——这也让下次添加同类型能复用它。

### 为什么要补发 Removed 信号

`GameEffects::GameEventHandler::OnGreatWorkRemoved_DetachGameEffects` 订阅的是
`Signals+0x19C8`，但**全库没有任何发射点**（引擎自己的 Deal Enact、间谍窃取都只调
`RemoveGreatWork` 就结束）。不补发的话，该巨作之前挂上的 Modifier 会永久留在城市上：
「加了删、删了加」能把同一份加成刷成多份。见
[GreatWorkModifierAttach](../../BugFix/GreatWorkModifierAttach/README.md)。

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
2. 若 i 已经被【任意玩家的任意城市】占用：
     a. 用一个列表里不存在的【假类型】调 FindOrAddGreatWork
        —— 查找必然未命中，引擎走自己的 DoInsertValue 追加路径
     b. 重新取新条目地址（可能因扩容而移动），把类型改成目标类型
        返回新下标 j
     c. 造不出来就返回 -1，【绝不退回】占用中的旧索引
3. SetGreatWorkPlayer(j, playerId)
```

这样完全复用引擎的追加逻辑（内存管理、字段初始化、容量增长全部正确），
不需要手工构造 `GreatWork`、不需要调 `DoInsertValue`、不需要 hook 任何函数。

### 判重范围必须是全局，不是本城

早期版本用 `IndexInUse(buildings, i)` 只扫目标城市的槽，理由是
「`RemoveGreatWork` 也只在单个 `Buildings` 内扫，所以建筑内唯一就够」。
这个理由是**错的** —— 漏掉了两件事：

| 遗漏点 | 后果 |
| --- | --- |
| `greatWorks` 列表是 `Game::Culture` 级别、全局唯一 | 同一个 index 出现在两座城市 = 同一件巨作被复制成两份 |
| 条目里的 `player` 字段也只有一个 | **跨玩家**添加时 `SetGreatWorkPlayer` 会把前一件的所有者改掉 |

具体复现：玩家1 城市A 加类型 42 → index 0（owner=1）；
玩家2 城市B 再加类型 42 → `FindOrAddGreatWork` 仍返回 0（按 type 命中），
本城检查又是 false → 复用 0 → owner 被改成 2。城市A 那件巨作被"过户"了。

所以现在改用 `IndexInUseAnywhere(i)`，遍历所有玩家的所有城市：

```c
for (int playerId = 0; playerId < kMaxPlayerScan; ++playerId) {
    void* player = g_api->GetPlayer(playerId);
    if (!player) continue;
    // player -> Cities 链表 -> 每个 City 的 Buildings -> 槽
    // 命中 return true
}
```

> 语义边界：「搬移既有巨作」**不经过**这里 —— `MOVE_GREAT_WORK` / 迁都 /
> 建筑搬迁走引擎自己的 `Process` / `RelocateBuildingGreatWorks`，它们必须沿用
> 原 index（本来就是同一件巨作换位置）。本函数只服务「新增」。

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
| `Culture::FindOrAddGreatWork` | `HookName` + AOB | 22 字节，唯一 |
| `Culture::SetGreatWorkPlayer` | `HookName` + AOB | 19 字节，唯一 |
| `Buildings::CanAddGreatWork` | `HookName` + AOB | 38 字节，唯一 |
| `Buildings::AddGreatWork` | `HookName` + AOB | 31 字节，唯一 |
| `Buildings::AddGreatWorkToSlot` | `HookName` + AOB | 22 字节，唯一（仅保留解析与就绪判定，不用于放置） |
| `Player::Districts_Units_Cities::Find` | `HookName` + AOB | 22 字节，唯一 |
| `Buildings::RemoveGreatWork` | `HookName` + AOB | 24 字节，唯一（仅用于 Remove） |

> AOB 字节数以 `GreatWorkAPI.cpp` 里 `HookName(sym, "...")` 的实际串为准；
> 改代码时同步本表，不要凭记忆填。

## 注册环境

只注册到 **gameplay** 脚本环境 —— 这三个接口操作的是引擎 gameplay 域对象
（`Culture` / `Buildings`），UI 脚本环境用不到，也不该在那里暴露。

## 日志

```
[GreatWorkAPI] symbols ok; GreatWorkManager.AddGreatWork/AddGreatWorkToCity/RemoveGreatWork registered
[GreatWorkAPI] AddGreatWorkToCity ok: player=0 city=65536 gw=0 list=0
[GreatWorkAPI] duplicate great work: gw=0 reuse=0 -> new=5
[GreatWorkAPI] RemoveGreatWork ok: player=0 gw=0 list=0 slot=1
[GreatWorkModifierAttach] GreatWorkRemoved dispatched: gw=0
```

`duplicate great work` 出现说明触发了多份支持：`reuse` 是原本会复用的 index，
`new` 是新分配的唯一 index。