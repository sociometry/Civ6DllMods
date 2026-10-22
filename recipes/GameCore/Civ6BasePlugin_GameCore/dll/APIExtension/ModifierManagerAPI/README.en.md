# ModifierManagerAPI

> English | [中文](README.md)

> Category: **APIExtension** · Back to [project overview](../../../README.md)

Adds "attach / detach Modifier" Lua interfaces for mod authors.

## Why it is needed

Vanilla Lua can attach Modifiers to units, but:

- **Cannot detach** — there is no corresponding Detach interface;
- **Cannot count** — there is no way to know how many stacks of a given ID are currently attached;
- **Limited to one identity** — only units can use it; cities / players / districts cannot.

## Interface

```lua
-- attach (instance method, injected into IUnit)
unit:AttachModifierByID(modifierID)                 --> number   -- returns the index, -1 on failure

-- detach (owner may be unit / city / player / district)
ModifierManager.DetachModifierByID(owner, id[, n])  --> number   -- returns the number of layers actually removed (0 = not found); n defaults to removing one layer

-- count
ModifierManager.GetCount(owner, id)                 --> number
```

When `n` exceeds the actual number of layers, all are removed. Repeated detachment returns `0` instead of raising an error (note that the return value is a **layer count**, not a boolean).

```lua
local idx = unit:AttachModifierByID("COMMANDO_BONUS_MOVEMENT")
print(ModifierManager.GetCount(unit, "COMMANDO_BONUS_MOVEMENT"))  -- 1
ModifierManager.DetachModifierByID(unit, "COMMANDO_BONUS_MOVEMENT", 1)
print(ModifierManager.GetCount(unit, "COMMANDO_BONUS_MOVEMENT"))  -- 0
```

## Core design: Detach does not rely on its own ledger

This is the most critical decision in this module. **It does not record "what I attached"; it asks the engine what currently exists**:

```
MakeObject(owner)                    -> shared_ptr<IModifierObject>
GetModifiersOwnedBy(system, outVec, sp)  -> list of owned indexes
compare IDs one by one -> ClearModifier(system, index) -> IModifier::Deactivate
```

The benefit is:

| Scenario | Can it be detached correctly |
| --- | --- |
| Layers attached by the engine itself | ✅ |
| Layers attached by other plugins | ✅ |
| Layers left over across save files | ✅ |
| Layers attached through other paths (effects / events) | ✅ |

**It does not matter who attached it** — as long as it currently exists in the engine's Modifier system, it can be found and removed.

## Pitfall hit: only ClearModifier without Deactivate

`ClearModifier` only removes the index from the map and **does not revoke the property bonus**.

Observed behavior: after attaching `+1 movement` and then detaching, `GetMaxMoves()` still stayed at 5.

Every one of the engine's own Remove paths does **`IModifier::Deactivate` first and then `ClearModifier`**;
this module follows that, and the problem disappeared.

## Overloads for the four owner types

Each owner type has its own `MakeObject` and `GetInstance`:

| owner | MakeObject | GetInstance |
| --- | --- | --- |
| Unit | separate overload | `ScopedInstance<IUnit,...>::GetInstance` |
| City | separate overload | `ScopedVirtualInstance<ICity,...>::GetInstance` |
| Player | separate overload | `IPlayer::GetPlayerInstance` (**not** a template instantiation) |
| District | separate overload | `ScopedInstance<IDistrict,...>::GetInstance` |

> Player cannot use `ScopedVirtualInstance<IPlayer,PlayerReference>`: that only returns a `PlayerReference` (an ID reference),
> it cannot get `Player::Instance*`, which does not match the parameter of `MakeObject(Player)`.

For overloads the symbol DB only gives pure ordinals such as `_1` / `_2` / `_10` (`MakeObject_10` gives no hint at all that it is the one taking
`Unit`), so **targets with overloads always stay mangled**, which is instead more readable and more stable.

## Targets

| Target | Method | AOB |
| --- | --- | --- |
| `ModifierSystem::AttachModifier` | `HookName` + AOB | 18 bytes, unique |
| `ModifierSystem::ClearModifier` | `HookName` + AOB | 19 bytes, unique |
| `GetModifiersOwnedBy` | `HookName` + AOB | 17 bytes, unique |
| `GetModifierOwnedByCount` | `HookName` + AOB | 16 bytes, unique |
| `Game::Instance::Edit` | `HookName` + AOB | 19 bytes, unique |
| `Game::Instance::EditGameEffects` | `HookName` + AOB | 16 bytes, unique |
| `Handle::get` | `HookName` + AOB | 23 bytes, unique |
| `ScopedInstance<*>::GetInstance` (Unit/District) | `HookName` | **none** (template instantiation prefixes are not unique) |
| `ScopedVirtualInstance<ICity>::GetInstance` | `HookName` | **none** (same as above) |
| `IPlayer::GetPlayerInstance` | `HookName` | **none** (prefix not unique, and it is not a template instantiation) |
| `GameEffects::MakeObject` (4 overloads) | **mangled** `HookName` | Unit/City/Player **none** (their heads are byte-for-byte identical, 3 hits); **District carries a 16-byte AOB** (unique hit) |
| `std::string::assign` | `HookName` | **none** (prefix not unique) |

> The hit counts follow the measured values recorded in the top comment of `ModifierManagerAPI.cpp` (the ScopedInstance family **731** hits,
> `IPlayer::GetPlayerInstance` **109** hits, `std::string::assign` **82** hits, `MakeObject(Unit/City/Player)` **3** hits).
> The AOB byte counts are governed by the actual strings of `HookName(sym, "...")` in the same file; sync this table when changing the code, do not fill it in from memory.

## Injection method

`unit:AttachModifierByID` is an **instance method**, injected through `PushLuaMethods` into
the PushMethods of `GameCore::Lua::IUnit`:

```cpp
PushLuaMethods("GameCore::Lua::IUnit::PushMethods", kHookApiPushNoManager, &s_attachModifier, 1);
```

## Origin

Migrated from `Unit.cpp` of CivilizationVI_CommunityExtension 1.3.0-beta
(`lAttachModifierByID` / `lDetachModifierByID`) to the Civ6DllLoader plugin framework.

## Log

```
[ModifierManager] unit-method inject: reg=1 pushed=1 (unit:AttachModifierByID(id))
[ModifierManager] symbols resolved; unit:AttachModifierByID available
[ModifierManager] attach ok: unit=000000023AD9BE18 id=COMMANDO_BONUS_MOVEMENT idx=823
[ModifierManager] detach ok: id=COMMANDO_BONUS_MOVEMENT idx=823
[ModifierManager] attach failed: unit=... id=MODIFIER_DOES_NOT_EXIST_XYZ status=1
```
