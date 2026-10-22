# GreatWorkTypeLookup

> English | [中文](README.md)

> Category: **BugFix** · Back to [Project overview](../../../README.md)

Let Great Work types and Great Work slot types added by mods be recognized by the engine.

## Problem

The engine implements the “name → type index” conversion as **two hardcoded switch tables**:

- `Definition::Utility::NameToGreatWorkSlotType` — name hash → slot type
- `Definition::Utility::NameToGreatWorkObjectType` — name hash → object type

The `default` branch of both tables returns `-1` directly. Thus:

| Symptom | Root cause |
| --- | --- |
| Great Work types added by mods (e.g. a custom “sculpture”) cannot go into any slot | ObjectType cannot be found → `IsValidObjectForSlot` is always false |
| Slot types added by mods (e.g. a custom “display cabinet”) cannot be created | SlotType returns -1 → `AddGreatWorkSlots` skips that definition |

## Approach

The engine actually **has another data-driven path**: `Types::GetIndex(type)` — it looks up by hash in the definition set
and returns the index when it hits. This path supports modded content by nature.

So the fix is: **hook those two hardcoded switches and, when they return -1, switch to the data-driven resolver**.

```
vanilla: name hash → [hardcoded switch] → index or -1
this module: name hash → [hardcoded switch] → index
                                                  ↓ (when -1)
                                            [Types::GetIndex] → index or -1
```

## Hardcoded tables verified by decompilation

**Slot types** (`NameToGreatWorkSlotType`):

| Name | Hash |
| --- | --- |
| MUSIC | `0x2F6C7BC9` |
| RELIC | `0x8ABFC848` |
| PALACE | `0xC8609506` |
| HERO | `0xD8878949` |
| WRITING | `0x00B22BE8` |
| PRODUCT | `0x3E60D2DE` |
| ARTIFACT | `0x6868A539` |
| ART | `0x699389CB` |
| CATHEDRAL | `0x6DB6854C` |

**Object types** (`NameToGreatWorkObjectType`):

| Name | Hash |
| --- | --- |
| SCULPTURE | `0x05AFB7A8` |
| PRODUCT | `0x8869D47F` |
| RELIGIOUS | `0xACA72322` |
| MUSIC | `0xB487A51F` |
| WRITING | `0xB6BB2D49` |
| ARTIFACT | `0xC90F3F41` |
| RELIC | `0x1154169E` |
| PORTRAIT | `0x14AA6B96` |
| LANDSCAPE | `0x1E0E0BF4` |
| HERO | `0x579BEF89` |

These two tables themselves are **not modified** — the module takes over only when they return `-1`, so the resolution path for vanilla content is completely unchanged.

## Scope of benefit

Four call surfaces:

| Function | Impact |
| --- | --- |
| `Buildings::AddGreatWorkSlots` | whether new slot types can be created |
| `Culture::IsValidObjectForSlot` | whether new Great Works can go into slots |
| `AI::GreatWorkBuildingInfo` / AI ValueManager | whether the AI can value them correctly |
| `Effects::AdjustExtraGreatWorkSlots::Initialize` | whether the slot-count effect can act on new slots |

On the object type side it additionally covers `Culture::Reset`, `Religion::GetNearestRelicSlot`,
the `Congress ResolutionGreatWorkObjectTarget` family, `AdjustGreatWorkObject*` effects, and so on.

## Targets

| Target | Symbol form | Notes |
| --- | --- | --- |
| `NameToGreatWorkSlotType` | `HookName` (demangled) + AOB | 16 bytes, unique |
| `NameToGreatWorkObjectType` | `HookName` (demangled) + AOB | 16 bytes, unique |
| `Types::GetIndex<GreatWorkSlotTypes>` | **MSVC mangled name** | the demangled name carries a `_26` ordinal that changes across versions |
| `Types::GetIndex<GreatWorkObjectTypes>` | **MSVC mangled name** | same as above |

The resolver does not use demangled names because the symbol DB gives template instances only a plain ordinal such as `Types::GetIndex_26`,
and the ordinal drifts as unrelated code is added or removed; the mangled name is instead the stable full name.

## Cache

Resolution results go into a cache table with a **capacity cap** (cap 4096), so that abnormal hashes cannot blow the table up.

## Source

The slot type part is migrated from `Main.cpp` of CivilizationVI_CommunityExtension 1.3.0-beta
(`PatternTest` + `Hook_S`); the object type part is extended following the same defect pattern (not covered by CE).

## Logs

```
[GreatWorkTypeLookup] GreatWorkSlotType hooked; resolver=ok (modded types now resolvable)
[GreatWorkTypeLookup] GreatWorkObjectType hooked; resolver=ok (modded types now resolvable)
```

When the resolver is unavailable it degrades to “use the hardcoded table only”, and vanilla behavior is unchanged:

```
[GreatWorkTypeLookup] %s: data-driven resolver unavailable - falling back to the hardcoded table only
```
