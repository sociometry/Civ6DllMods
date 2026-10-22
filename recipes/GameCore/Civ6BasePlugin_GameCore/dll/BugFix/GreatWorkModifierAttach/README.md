# GreatWorkModifierAttach

> 中文 | [English](README.en.md)

> 分类：**BugFix** · 返回 [工程总览](../../../README.md)

让伟人创作出来的巨作能触发挂在它身上的 Modifier。

## 问题

伟人（大作家 / 大艺术家 / 大音乐家 / 大考古学家）创作出来的巨作，
不会触发 Database 里针对该 GreatWork 配置的 Modifier；
而同样的巨作由 Product（大商人随机产品）产出时一切正常。

## 定位

巨作挂 Modifier 的唯一入口是 GameEffects 侧的这个回调：

```c
GameEffects::GameEventHandler::OnGreatWorkAdded_AttachGameEffects(
    GameCore::City::Instance* city, GameCore::GreatWorkTypes type);
```

它在 `GameEffects::GameEventHandler` 的构造函数里订阅到一条 GameCore 内部信号：

```asm
lea r8, OnGreatWorkAdded_AttachGameEffects
lea rcx, [r14+19B0h]          ; Game::Signals + 0x19B0
call ProtoSignal<...>::Subscribe
lea r8, OnGreatWorkRemoved_DetachGameEffects
lea rcx, [r14+19C8h]          ; Game::Signals + 0x19C8
```

所以「谁发了 `Signals+0x19B0` 这条 Dispatch，谁才能挂上 Modifier」。

把库里 **75 个** `ProtoSignal::Dispatch` 调用点逐个回溯偏移后统计：

| 偏移 | 发送方 | 场景 |
| --- | --- | --- |
| **`+0x19B0`** | `Operations::Manager::Process` | 移动 / 交换巨作 |
| **`+0x19B0`** | `GrantRandomProduct` | Product（大商人） |
| `+0x19C8` | 同上两处 | 对应的解除 |
| `+0x240` | `Buildings::AddGreatWork` | **另一条信号**，与挂 Modifier 无关 |

结论：`+0x19B0` 全库只有 **2 处** 会发。

再看普通创作路径：

```
ActivateGreatPerson::Start
  └─ Game::Culture::AddGreatWorkToTile
       └─ Buildings::AddGreatWorkToBuilding      ← 唯一落槽出口
```

这条链从头到尾**只调了** `Reporting::Event::Send(GREAT_WORK_CREATED)`——
那是发给 UI 的**上报事件**（`GreatWorkShowcase.lua` / `LaunchBar.lua` 在用），
跟 GameEffects 的 Modifier 完全是两套东西。真正该发的 `+0x19B0` 从没发过。

Product 之所以正常，正是因为 `GrantRandomProduct` 自己补了这一次 Dispatch。

## 做法

### 伟人激活

hook `Buildings::AddGreatWorkToBuilding`，原函数成功返回（`>= 0`）后补一次 Dispatch：

```c
int Hook_AddGreatWorkToBuilding(Buildings* b, int individualType,
                                uint64_t plotCoord, int* outBuildType) {
    int added = g_origAddToBuilding(b, individualType, plotCoord, outBuildType);
    if (added >= 0) {
        void* city    = *(void**)b;                              // Buildings + 0x00 = m_pCity
        char* signals = *(char**)((char*)Game::Instance::Edit() + 0x280);
        g_dispatch(signals + 0x19B0, city, added);
    }
    return added;
}
```

### Lua 接口

`GreatWorkAPI` 在自己放置成功后调用导出函数：

```c
g_addGreatWork(buildings, listIndex);          // Buildings::AddGreatWork
NotifyGreatWorkAdded(culture, buildings, listIndex);
```

注意 `NotifyGreatWorkAdded` 内部还要做一次下标翻译：

```c
// AddGreatWork 收的是 greatWorkListIndex，而信号要的是 greatWorkType。
// 同一 greatWorkType 可以有多条列表条目（同种巨作的第 2、3… 份），
// 所以必须回查 Culture 列表才能真正拿到类型。
int GreatWorkTypeOfListIndex(void* culture, int listIndex) {
    char* entry = GetGreatWorkEntry(culture, listIndex);
    return entry ? *(int*)(entry + OFF_GW_TYPE) : -1;
}
```

若模块未加载 / 符号解析失败，`GreatWorkModifierAttach_NotifyAdded` 是空实现，
接口主流程不受影响。

### 覆盖两条路径

游戏里有两条「新巨作诞生」的路，本模块分别用不同手段覆盖：

| 路径 | 手段 |
| --- | --- |
| 伟人激活：`AddGreatWorkToTile` → `AddGreatWorkToBuilding` | **hook** 该函数，成功后补发 |
| 本工程 Lua 接口：`GreatWorkManager.AddGreatWork` / `AddGreatWorkToCity` | 放置成功后**主动调用** `GreatWorkModifierAttach_NotifyAdded()` |

第二条为什么不 hook 而要导出函数：它走的是 `Buildings::AddGreatWork`，而这个函数
**有 5 个调用者，没有一个是"新创作"**：

| 调用者 | 场景 |
| --- | --- |
| `MoveCapital` / `FindandMovetoNewCapital` | 迁都搬移 |
| `RelocateBuildingGreatWorks` | 建筑搬迁 |
| `ReceiveGreatWork` | 收到巨作 |
| `Player::Cities::AddGreatWork` | 谍报窃取 |

在这些路径上补 Added 会让新城市重复挂载，而对应的 Removed 信号
（`Signals+0x19C8`）**全库无人发射**，旧城市那份永远解不掉，越搬越多。
所以只能由「确实新增」的调用方显式通知。

### 其它候选

| 候选钩子 | 结论 | 理由 |
| --- | --- | --- |
| `AddGreatWorkToSlot` | 不钩 | 公共落点，Product / Process 已各自发过，钩它会**双倍挂载** |

## 踩过的坑：ICF 让符号名说谎

库里这条 Dispatch 的符号名是：

```
?Dispatch@?$ProtoSignal@$$A6AXW4PlayerTypes@GameCore@@PEBVInstance@City@2@@Z...
```

看起来像是 `void(PlayerTypes, City const*)` 的实例，但实际发射点的寄存器用法是：

```asm
mov rdx, [rbp+playerID]     ; rdx = City::Instance*
mov r8d, ebx                ; r8d = GreatWorkTypes
call Dispatch
```

原因：链接器的 ICF（identical code folding）把参数**形状相同**的 Dispatch 实例
合并成了同一份机器码，符号名只是"谁先被链接到"的结果。

Template 的 ABI 只跟形参的寄存器和个数有关，与具体类型无关：

| 寄存器 | 含义 |
| --- | --- |
| `rcx` | `ProtoSignal*` |
| `rdx` | 第 1 实参（这里是 `City::Instance const*`） |
| `r8d` | 第 2 实参（这里是 `GreatWorkTypes`） |

所以按 `(signal, city, greatWorkType)` 声明并调用即可，不必纠结符号名。
这一点已用「`Dispatch` 头 29 字节与另一个实例逐字节相同，第 30 字节才分叉」验证。

## 目标

| 目标 | RVA | 取址方式 | AOB 兜底 |
| --- | --- | --- | --- |
| `City::Buildings::AddGreatWorkToBuilding` | `0x109890` | 符号名 | 无（该序言库里有 2 处命中） |
| `ProtoSignal<...>::Dispatch` | `0xDA700` | 符号名 | 无（ICF，AOB 有 2 处命中） |
| `Game::Instance::Edit` | `0x951550` | 符号名 | `48 8B 05 ?? ?? ?? ?? C3` |

> `Game::Instance::Edit` 只有一条 `mov rax, cs:ms_pkCurrentGame; retn`，
> 直接返回全局指针，不需要拿 `Game::Instance*` 入参。

模块还导出一个 C 函数供本工程其它模块使用：

| 导出 | 用途 |
| --- | --- |
| `GreatWorkModifierAttach_NotifyAdded(buildings, greatWorkType)` | 由 `GreatWorkAPI` 在"确实新增"后调用 |

## 验证

Loader 日志：

```
[GreatWorkModifierAttach] active: great works from Great Person now fire GreatWorkAdded
```

游戏内：让大作家创作一件著作，该城的 Modifier 面板应出现对应加成。
若日志变成 `... unresolved - skipped`，说明游戏更新后符号漂移，本模块自动跳过，
**不影响其它模块**。