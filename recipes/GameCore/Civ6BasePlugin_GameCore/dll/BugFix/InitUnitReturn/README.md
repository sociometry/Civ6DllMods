# InitUnitReturn

> 分类：**BugFix** · 返回 [工程总览](../../../README.md)

让 `UnitManager.InitUnitValidAdjacentHex` 返回它建出来的那个单位。

## 问题

```lua
local u1 = UnitManager.InitUnit(62, unitType, x, y)
print(u1)   -- 有值

local u2 = UnitManager.InitUnitValidAdjacentHex(62, unitType, x, y, 3)
print(u2)   -- nil
```

两个函数都声明返回 1 个值，但只有前者真的往 Lua 栈压了对象。

## 定位

两个函数的尾部（IDA 反编译）：

**`lInitUnit`（RVA `0x701480`）—— 正常**

```c
GameCore::Message::SignalBase::ProtoSignal<...>::Dispatch(...);
GameCore::Lua::ScopedInstance<GameCore::Lua::IUnit, GameCore::Unit::Instance>
    ::Push(lua_State, value: v12);       // ← 压了
return 1;
```

**`lInitUnitValidAdjacentHex`（RVA `0x7015C0`）—— 缺**

```asm
172681733  mov rcx, rbx          ; Unit::Instance*
172681736  call Unit::Placement::Set
17268173b  mov rcx, cs:ms_pkCurrentGame
172681745  mov rdx, rbx          ; Unit::Instance*
172681756  call ProtoSignal<...>::Dispatch
17268175b  mov eax, 1            ; ← 声明返回 1 个值
172681760  add rsp, 80h
17268176b  retn
```

**全程没有 Push**。Lua 按「返回 1 个值」取栈，栈上什么都没有 → `nil`。

## 做法

难点：`Unit::Instance*` 只活在原函数的 `rbx` 寄存器里，
既不作为返回值传出，也没有可写入的出参——detour 拿不到。

所以分两段：

```
Hook A ─ lInitUnitValidAdjacentHex
   入口：记下 L、记下栈顶，active = true
   调用原函数
          │
          │  Hook B ─ Unit::Placement::Set(unit, coord, 0, 0)
          │      第一个参数就是该单位实例，active 为真时截下来
          ▼
   出口：active = false
        若原函数声称返回 N 个值、但栈顶没涨够 → 用截到的实例补 N 个 Push
```

### 为什么不在 Placement::Set 里直接 Push

`Placement::Set` 是全局工具函数，别处也在用。而且原函数在 Set 之后还要
`Dispatch(UnitPlaced)` 并收尾——若在 Set 里就 Push，将来引擎自己修好了，
就会返回两个值，Lua 侧同样出错。

放在 A 的出口补，既能确认「原函数确实没压」，顺序又天然在 Dispatch 之后，
与 `lInitUnit` 的既有行为一致。

### 自带退化保护

```c
const int topAfter = g_api->LuaGetTop(L);
if (topAfter - topBefore >= pushed) {
    return pushed;        // 原函数自己压过了 —— 什么都不做
}
```

如果引擎哪天把这一行补上了，本模块自动退化为空操作，**不会变成返回两个值**。

## 目标

| 目标 | RVA | 取址方式 | AOB 兜底 |
| --- | --- | --- | --- |
| `Lua::IUnitManager::lInitUnitValidAdjacentHex` | `0x7015C0` | 符号名 | `40 53 55 41 56 48 81 EC 80 00 00 00 BA 01 00 00 00 48 8B D9`（唯一） |
| `Unit::Placement::Set` | `0x3B11B0` | 符号名 | `40 55 57 41 54 41 55 48 8D AC 24 48 FF FF FF 48 81 EC B8 01 00 00`（唯一） |
| `ScopedInstance<IUnit, Unit::Instance>::Push` | `0x1EFF0` | 符号名 | 无（模板实例，该序言库里有 10 处命中） |

## 验证

Loader 日志：

```
[InitUnitReturn] active: InitUnitValidAdjacentHex now returns the unit instance
```

首次触发补 Push 时还会打印一行（每次调用一条）：

```
[InitUnitReturn] InitUnitValidAdjacentHex returned nil - pushed unit instance
```

游戏内：

```lua
local u = UnitManager.InitUnitValidAdjacentHex(62, unitType, x, y, 3)
print(u)             -- 应为一个 table（Unit 实例），不再是 nil
print(u:GetX(), u:GetY())
```

## 备注

`Placement::Set` 全库有别的调用者，本模块只在 `active` 为真（即确实位于
`lInitUnitValidAdjacentHex` 调用链内）时才记录实例，其余调用只做透传。
标记位用 `thread_local`，即使出现嵌套也不会串味。