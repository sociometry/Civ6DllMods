# InitUnitReturn

> English | [中文](README.md)

> Category: **BugFix** · Back to [Project Overview](../../../README.md)

Make `UnitManager.InitUnitValidAdjacentHex` return the unit it just created.

## Problem

```lua
local u1 = UnitManager.InitUnit(62, unitType, x, y)
print(u1)   -- has a value

local u2 = UnitManager.InitUnitValidAdjacentHex(62, unitType, x, y, 3)
print(u2)   -- nil
```

Both functions declare one return value, but only the former actually pushes an object onto the Lua stack.

## Diagnosis

The tails of the two functions (IDA decompilation):

**`lInitUnit` (RVA `0x701480`) — normal**

```c
GameCore::Message::SignalBase::ProtoSignal<...>::Dispatch(...);
GameCore::Lua::ScopedInstance<GameCore::Lua::IUnit, GameCore::Unit::Instance>
    ::Push(lua_State, value: v12);       // ← pushed
return 1;
```

**`lInitUnitValidAdjacentHex` (RVA `0x7015C0`) — missing**

```asm
172681733  mov rcx, rbx          ; Unit::Instance*
172681736  call Unit::Placement::Set
17268173b  mov rcx, cs:ms_pkCurrentGame
172681745  mov rdx, rbx          ; Unit::Instance*
172681756  call ProtoSignal<...>::Dispatch
17268175b  mov eax, 1            ; ← claims one return value
172681760  add rsp, 80h
17268176b  retn
```

**There is no Push anywhere.** Lua reads the stack expecting one return value, the stack holds nothing → `nil`.

## Approach

The difficulty: `Unit::Instance*` lives only in the original function's `rbx` register;
it is neither passed out as a return value nor exposed through a writable out-parameter — so a detour cannot get it.

So it is split into two stages:

```
Hook A ─ lInitUnitValidAdjacentHex
   entry: record L, record the stack top, active = true
   call the original function
          │
          │  Hook B ─ Unit::Placement::Set(unit, coord, 0, 0)
          │      the first argument is exactly that unit instance; capture it while active is true
          ▼
   exit: active = false
        if the original function claims N return values but the stack top has not grown enough → make up the N pushes from the captured instance
```

### Why not push directly inside Placement::Set

`Placement::Set` is a global utility function that other code uses too. Moreover, after Set the original function still has to
`Dispatch(UnitPlaced)` and wrap up — if we pushed inside Set, then once the engine fixes this itself,
it would return two values, which breaks the Lua side just the same.

Doing the supplementary push at A's exit both confirms that the original function really did not push anything, and places it
naturally after Dispatch, consistent with the existing behavior of `lInitUnit`.

### Built-in degradation guard

```c
const int topAfter = g_api->LuaGetTop(L);
if (topAfter - topBefore >= pushed) {
    return pushed;        // the original function pushed on its own — do nothing
}
```

If the engine ever fills in that line, this module automatically degrades to a no-op and **will not start returning two values**.

## Targets

| Target | RVA | Address resolution | AOB fallback |
| --- | --- | --- | --- |
| `Lua::IUnitManager::lInitUnitValidAdjacentHex` | `0x7015C0` | symbol name | `40 53 55 41 56 48 81 EC 80 00 00 00 BA 01 00 00 00 48 8B D9` (unique) |
| `Unit::Placement::Set` | `0x3B11B0` | symbol name | `40 55 57 41 54 41 55 48 8D AC 24 48 FF FF FF 48 81 EC B8 01 00 00` (unique) |
| `ScopedInstance<IUnit, Unit::Instance>::Push` | `0x1EFF0` | symbol name | None (template instantiation; that prologue has 10 hits in the library) |

## Verification

Loader log:

```
[InitUnitReturn] active: InitUnitValidAdjacentHex now returns the unit instance
```

The first time a supplementary push is triggered, one more line is printed (one per call):

```
[InitUnitReturn] InitUnitValidAdjacentHex returned nil - pushed unit instance
```

In game:

```lua
local u = UnitManager.InitUnitValidAdjacentHex(62, unitType, x, y, 3)
print(u)             -- should be a table (a Unit instance), no longer nil
print(u:GetX(), u:GetY())
```

## Notes

`Placement::Set` has other callers throughout the library; this module records the instance only while `active` is true (that is,
while it really is inside the `lInitUnitValidAdjacentHex` call chain) and merely passes every other call through.
The flag is `thread_local`, so even nested calls do not bleed into each other.
