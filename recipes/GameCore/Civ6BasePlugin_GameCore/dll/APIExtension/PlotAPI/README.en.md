# PlotAPI

> English | [中文](README.md)

> Category: **APIExtension** · Back to [project overview](../../../README.md)

Adds a Lua interface for mod authors to modify a plot's Appeal.

## Why it is needed

Vanilla Lua can only read Appeal (`Plot:GetAppeal()`); there is no write path.

## Interface

```lua
Plot:ChangeAppeal(n)   --> boolean
```

| Parameter | Meaning |
| --- | --- |
| `n` | **Relative delta**, positive increases and negative decreases. `double` is truncated to an integer |

**Accumulating semantics**: repeated calls on the same plot add up rather than overwrite. Not clamped (may go negative).

```lua
local pPlot = Map.GetPlot(10, 10)
print(pPlot:GetAppeal())     -- original value a
pPlot:ChangeAppeal(3)
print(pPlot:GetAppeal())     -- a + 3
pPlot:ChangeAppeal(-3)
print(pPlot:GetAppeal())     -- back to a
```

## Why "ledger + read interception" instead of changing real data

Appeal **has no writable real field**. `Plot::Instance::GetAppeal` does:

```c
if (this->vtable[0x30]()->byte[0x1C] != 0xFF) return *(int16*)(this + 0x4A);  // cache
return Rules::Appeal::Get()->GetAppeal(this);                                 // computed
```

The computed path reads terrain / features / adjacency / buildings and cannot be edited;
the cache field (`+0x4A`) is only used under specific conditions and is overwritten by the
next recomputation — writing it is both unstable and save-polluting.

So the delta lives in a plugin-side ledger and is added back on read.

## Where the read interception is installed

On the **body** of `Plot::Instance::GetAppeal` (vtable slot 5 / `+0x28`), not on the 6-byte
trampoline that `lGetAppeal` goes through:

```asm
; trampoline (RVA 0x1A44C)
mov rax, [rcx]
jmp qword ptr [rax+28h]
```

The trampoline only catches callers that go through that stub; hooking the function body
catches every virtual call, including engine-internal ones. Lua's `Plot:GetAppeal` enters
that body through the trampoline (verified in `lGetAppeal`'s disassembly), so Lua reads
always see the offset.

```
Lua: Plot:GetAppeal()
  └─ IMapPlot::lGetAppeal
       └─ trampoline mov rax,[rcx]; jmp [rax+28h]
            └─ Plot::Instance::GetAppeal   ← this module's detour
```

## Ledger key: the plot index is computed, not stored

`Plot::Instance::GetIndex` does **not** read a field:

```c
index = (int16)(this + 0x28) + (int16)(this + 0x2A) * EditMap()->width(+0x28)
```

The ledger key must therefore come from calling that function, never from a raw offset.

## Ledger lifetime

This plugin is a **GameCore plugin**: it unloads together with the GameCore image when leaving a
game or loading a save, and `PlotAPI_Shutdown` clears the ledger — so plot deltas from a previous
game never leak into the next one.

Plot indices are only meaningful within one map, so switching maps **must** clear the ledger;
plugin unload guarantees that.

> The ledger is never persisted and never enters a save. After loading a save the deltas are gone
> and must be re-applied.

## Targets

| Target | Method | AOB |
| --- | --- | --- |
| `Map::Plot::Instance::GetAppeal` | `HookName` + AOB | 16 bytes, unique (hooked) |
| `Map::Plot::Instance::GetIndex` | `HookName` + AOB | 16 bytes, unique |
| `ScopedInstance<IMapPlot, Plot::Instance>::GetInstance` | `HookName` | **none** (prologue identical to the Unit / Governors argument getters) |

## Registration environment

Installed on the vanilla `IMapPlot` metatable via `PushLuaMethods`, **gameplay environment only**.
`sigType` is `kHookApiPushNoManager` (that function really is `PushMethods(L, stackOffset)`).

## Logging

```
[PlotAPI] active: ChangeAppeal pushed=1 (appeal hooked at 0x...)
[PlotAPI] ChangeAppeal: plot=1234 delta=3 -> +3
```
