# GovernorAPI

> English | [中文](README.md)

> Category: **APIExtension** · Back to [project overview](../../../README.md)

Adds a Lua interface for mod authors to *set* a player's available Governor points.

## Why it is needed

Vanilla Lua can only **read** Governor points (`GetGovernorPoints` / `GetGovernorPointsSpent`);
there is no write path at all. A mod that wants to grant a Governor title has to resort to
roundabout means such as faking events.

## Interface

```lua
Player:GetGovernors():SetAvailableGovPoints(n)   --> boolean
```

| Parameter | Meaning |
| --- | --- |
| `n` | Target **available** points. Negative values are clamped to 0. `double` is truncated to an integer (2.7 becomes 2) |

Invalid arguments (not a number, object unavailable) return `false` and **do not throw a Lua error**.

```lua
Players[0]:GetGovernors():SetAvailableGovPoints(3)
print(Players[0]:GetGovernors():GetGovernorPoints())   -- spent + 3
```

## Semantics: it sets the *available* points, not the total

The engine **only stores a total** (`Governors+0x118`); "available" is a difference computed on the UI side:

```
available = total - GetGovernorPointsSpent()
```

So "set available to n" is equivalent to "set the total to spent + n":

```
spent     = GetGovernorPointsSpent(govs)        // pure read: sums title counts over the Governor list
newTotal  = spent + n
delta     = newTotal - current total
ChangeGovernorPoints(govs, delta)               // hand it back to the engine
```

### Why go through the engine's ChangeGovernorPoints instead of writing the field

`Governors::ChangeGovernorPoints(int delta)` already performs every finishing step
(verified by disassembly):

| What it does | Consequence of writing the field directly |
| --- | --- |
| Only mutates when `IsMajor(pPlayer)` | City-states / barbarians would be modified too |
| Clamps the total on positive deltas | You would have to clamp yourself |
| Calls `HandleGovernorTitleMessaging()` | Title notifications would not refresh |
| Fires `Event::Send(0xDA3EF23D, playerID, delta)` | The UI would not refresh (you would have to forge the event hash) |

Writing the field directly therefore buys nothing and only adds side effects.
It is called even when `delta == 0` — the engine itself fires the event unconditionally,
and the UI relies on that to refresh.

## Memory layout (verified by disassembly)

```
Governors + 0x070 = Player::Instance*   pPlayer
Governors + 0x118 = int                 governorPointsTotal (spent + available)
```

## Targets

| Target | Method | AOB |
| --- | --- | --- |
| `Player::Governors::ChangeGovernorPoints` | `HookName` + AOB | 20 bytes, unique |
| `Player::Governors::GetGovernorPointsSpent` | `HookName` + AOB | 16 bytes, unique |
| `ScopedVirtualInstance<IPlayerGovernors, Governors>::GetInstance` | `HookName` | **none** (prologue identical to the Unit / Plot argument getters) |

## Registration environment

Installed on the vanilla `PlayerGovernors` metatable via `PushLuaMethods` (the Loader appends
it when the engine itself calls `IPlayerGovernors::PushMethods`), so it exists **only in the gameplay environment**.

⚠️ `sigType` must be `kHookApiPushWithManager`: that function's signature is
`PushMethods(this, L, stackOffset)` — the first argument `rcx` is a discarded object pointer and
`L` arrives in `rdx`. Choosing wrong does not raise an error, but every argument silently shifts.

## Logging

```
[GovernorAPI] active: SetAvailableGovPoints pushed=1
[GovernorAPI] SetAvailableGovPoints: spent=2 total 2 -> 5 (delta=3)
```
