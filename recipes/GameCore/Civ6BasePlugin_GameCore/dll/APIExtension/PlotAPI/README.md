# PlotAPI

> 分类：**APIExtension** · 返回 [工程总览](../../../README.md)

给模组作者补上「修改地块魅力」的 Lua 接口。

## 为什么需要

原版 Lua 只能读魅力（`Plot:GetAppeal()`），没有任何写入口。

## 接口

```lua
Plot:ChangeAppeal(n)   --> boolean
```

| 参数 | 含义 |
| --- | --- |
| `n` | **相对增量**，正增负减。`double` 按整数截断 |

**累加语义**：同一地块多次调用是累加，不是覆盖。不钳制（可为负）。

```lua
local pPlot = Map.GetPlot(10, 10)
print(pPlot:GetAppeal())     -- 原值 a
pPlot:ChangeAppeal(3)
print(pPlot:GetAppeal())     -- a + 3
pPlot:ChangeAppeal(-3)
print(pPlot:GetAppeal())     -- 回到 a
```

## 为什么用「账本 + 读拦截」而不改真实数据

魅力**没有可写的真实字段**。`Plot::Instance::GetAppeal` 内部：

```c
if (this->vtable[0x30]()->byte[0x1C] != 0xFF) return *(int16*)(this + 0x4A);  // 缓存
return Rules::Appeal::Get()->GetAppeal(this);                                 // 现算
```

现算路径会读地形 / 地貌 / 相邻 / 建筑等一大堆状态，改不了；缓存字段
（`+0x4A`）只在特定条件下生效，且会被下一次重算覆盖 —— 写它既不稳定也会污染存档。

所以增量存在插件自己的账本里，读取时叠加。

## 读拦截挂在哪里

挂 `Plot::Instance::GetAppeal` 的**函数本体**（vtable 槽 5 / `+0x28`），
而不是挂在 `lGetAppeal` 走的那条 6 字节跳板上：

```asm
; 跳板（RVA 0x1A44C）
mov rax, [rcx]
jmp qword ptr [rax+28h]
```

跳板只能拦到"走这个桩"的调用方；直接挂函数体，所有虚调用（含引擎内部）都会经过它，
覆盖率更高。Lua 侧的 `Plot:GetAppeal` 正是经该跳板进入这个函数体
（`lGetAppeal` 反汇编核实），所以 Lua 读一定能读到叠加值。

```
Lua: Plot:GetAppeal()
  └─ IMapPlot::lGetAppeal
       └─ 跳板 mov rax,[rcx]; jmp [rax+28h]
            └─ Plot::Instance::GetAppeal   ← 本模块 detour
```

## 账本的键：地块索引是现算的

`Plot::Instance::GetIndex` **不是**读某个字段：

```c
index = (int16)(this + 0x28) + (int16)(this + 0x2A) * EditMap()->width(+0x28)
```

所以账本键必须调这个函数取，不能直读偏移。

## 账本生命周期

本插件是 **GameCore 插件**，退局 / 读档时随 GameCore 映像一起卸载，
`PlotAPI_Shutdown` 里清空账本 —— 不存在"上一局的地块增量残留到这一局"。

地块索引只在同一张地图内有意义，因此**换图必须清空**；这一条由插件卸载保证。

> 账本不落盘，不进存档。读档后增量消失，需要重新施加。

## 目标

| 目标 | 方式 | AOB |
| --- | --- | --- |
| `Map::Plot::Instance::GetAppeal` | `HookName` + AOB | 16 字节，唯一（被 hook） |
| `Map::Plot::Instance::GetIndex` | `HookName` + AOB | 16 字节，唯一 |
| `ScopedInstance<IMapPlot, Plot::Instance>::GetInstance` | `HookName` | **不带**（与 Unit / Governors 取参函数序言逐字节相同） |

## 注册环境

用 `PushLuaMethods` 挂到原版 `IMapPlot` 元表上，**只存在于 gameplay 环境**。
`sigType` 是 `kHookApiPushNoManager`（该函数签名就是 `PushMethods(L, stackOffset)`）。

## 日志

```
[PlotAPI] active: ChangeAppeal pushed=1 (appeal hooked at 0x...)
[PlotAPI] ChangeAppeal: plot=1234 delta=3 -> +3
```
