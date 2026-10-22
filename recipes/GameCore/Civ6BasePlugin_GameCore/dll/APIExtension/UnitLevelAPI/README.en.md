# UnitLevelAPI

> English | [中文](README.md)

> Category: **APIExtension** · Back to [project overview](../../../README.md)

Adds `SetLevel` for units (the engine only has reads, no public writes).

## Why it is needed

The engine's Lua interface hangs all unit experience related methods under the `Unit::Experience` table:

`GetLevel` / `GetExperiencePoints` / `GetExperienceForNextLevel` / `ChangeExperience` /
`SetPromotion` / `SetVeteranName` …… **there is no method to write the level**.

The engine does have `Experience::SetLevel(int)` internally (RVA `0x3974A0`), but it is not exposed to Lua.

## Interface

```lua
unit:GetExperience():SetLevel(3)   --> boolean
```

The call form is fully consistent with the sibling interfaces (it hangs on `Unit::Experience`, not on `Unit`).

## Approach: copy the engine's own injection method

```cpp
PushLuaMethods("GameCore::Lua::IUnitExperience::PushMethods", kHookApiPushNoManager, &s_setLevel, 1);
```

## Only wraps one layer, does not change semantics

`SetLevel` only writes the level field (`+0x10`) and sends a `UNIT_EXPERIENCE_CHANGED` event,
**it does not touch accumulated experience** (`+0x0C`) — that is the engine's original behavior, and this module keeps it as-is.

Disassembly:

```asm
13f3b74a0  sub rsp, 38h
13f3b74a4  mov rax, [rcx]              ; m_pUnit
13f3b74aa  mov [rcx+10h], edx          ; m_iLevel = level
13f3b74b2  mov [rsp+..], 0A038D885h    ; UNIT_EXPERIENCE_CHANGED
13f3b74e7  call Event::Send
13f3b74f0  retn
```

### Why there is no need to care about "experience needed to level up"

The threshold is not a stored field; it is **computed on the fly** from the level:

```
3 * MULT * ((level + level²) / 2)     // the input is +0x10
```

So as soon as the level is written, the threshold is immediately that of the new level.

What UI / Lua reads is a cached copy at the Cache layer, which is driven to recompute by the `0xA038D885` event — and this event
is sent by `SetLevel` itself. **Therefore this module does not interfere with experience values, nor does it resend the event.**

## Deliberately minimal dependency surface

It resolves only **2** official functions:

| Purpose | Symbol | Method |
| --- | --- | --- |
| Write the level | `GameCore::Unit::Experience::SetLevel` | `HookName` (demangled) + AOB |
| Get the instance | `GameCore::Lua::ScopedInstance<GameCore::Lua::IUnitExperience,GameCore::Unit::Experience>::GetInstance` | `HookName` (demangled) + AOB |

One less resolution means one less place that "might fail to resolve".

## Targets

| Target | RVA | AOB |
| --- | --- | --- |
| `Experience::SetLevel` | `0x3974A0` | 16 bytes, unique |
| `Lua::ScopedInstance<IUnitExperience>::GetInstance` | `0x6FFED0` | **none** (resolved by symbol name only) |

> `GetInstance` is a template instantiation; `UnitLevelAPI.cpp` passes only one argument to `HookName(...)`, with no AOB fallback.

## Log

```
[UnitLevelAPI] experience-method inject: reg=1 pushed=1 (unit:GetExperience():SetLevel(n))
```
