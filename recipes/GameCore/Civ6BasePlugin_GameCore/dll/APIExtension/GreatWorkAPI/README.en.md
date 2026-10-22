# GreatWorkAPI

> English | [中文](README.md)

> Category: **APIExtension** · Back to [project overview](../../../README.md)

Adds the "add a Great Work" Lua interface for mod authors.

## Why it is needed

Vanilla Lua has **no** interface whatsoever for adding a Great Work to a city. A mod that wants to give the player a Great Work can only resort to
roundabout means such as triggering a Great Person or faking events, which is neither reliable nor able to specify the city / slot.

The engine does have a complete set of internal functions, but they are not exposed.

## Interface

```lua
-- automatic city selection: iterate the player's cities, put it into the first one that has room
GreatWorkManager.AddGreatWork(playerID, greatWorkIndex)          --> boolean

-- specified city
GreatWorkManager.AddGreatWorkToCity(playerID, cityID, gwIndex)  --> boolean
```

| Parameter | Meaning |
| --- | --- |
| `playerID` | player ID |
| `cityID` | city ID (used by `AddGreatWorkToCity`) |
| `greatWorkIndex` | **database Index** (`.Index` of `GameInfo.GreatWorks`), not `GreatWorkListIndex` |

Invalid arguments (negative numbers, wrong types) return `false` and **do not throw a Lua error**.

```lua
local gw = GameInfo.GreatWorks.GREATWORK_MICHELANGELO_1.Index
print(GreatWorkManager.AddGreatWorkToCity(playerId, cityId, gw))  --> true
```

## Internal call chain

```
Game::Culture::Edit()                        -> Culture&
Culture::FindOrAddGreatWork(culture, type)   -> listIndex
Culture::SetGreatWorkPlayer(culture, idx, playerId)
Buildings::CanAddGreatWork(buildings, idx, nullptr)  -> whether there is room
Buildings::AddGreatWork(buildings, idx)              -> actually places it (picks a valid slot automatically)
GreatWorkTypeOfListIndex(culture, idx)               -> looks back up greatWorkType
GreatWorkModifierAttach_NotifyAdded(buildings, type) -> notifies GameEffects to attach the Modifier
```

### What the last step is for

After the Great Work is placed, the GameEffects side has to be notified once, otherwise the Modifier attached to it will not take effect.
This step is provided by [GreatWorkModifierAttach](../../BugFix/GreatWorkModifierAttach/README.md)
(which also fixes the same defect in vanilla Great Person activation).

Note that `AddGreatWork` takes a `greatWorkListIndex`, whereas the signal wants a `greatWorkType` —
the 2nd and 3rd copies of the same Great Work type have different listIndexes but the same type, so a look-back translation through the Culture list is required:

```c
int GreatWorkTypeOfListIndex(void* culture, int listIndex) {
    char* entry = GetGreatWorkEntry(culture, listIndex);   // listIndex * 0x18
    return entry ? *(int*)(entry + OFF_GW_TYPE) : -1;      // OFF_GW_TYPE = 0x00
}
```

When the module is not loaded this notification is a no-op, and the main flow of the interface is unaffected.

### Why use `AddGreatWork` rather than `AddGreatWorkToSlot(..., -1)`

These two functions have different semantics:

| Function | Behavior |
| --- | --- |
| `AddGreatWork` | Calls `IsValidObjectForSlot` slot by slot to validate **slot category vs Great Work category**, and only places into a valid empty slot |
| `AddGreatWorkToSlot(idx, bldType, -1)` | Finds the **first empty slot** in that building's record and writes directly, **without validating the category** |

The latter would stuff a writing into an art slot and the like. Therefore placement uniformly goes through `AddGreatWork`.

## Support for multiple copies of the same Great Work

### Problem

`Culture::FindOrAddGreatWork` **looks up** by `greatWorkType`, and the same type always returns the same `listIndex`.

So "multiple copies of the same Great Work" = the same index appearing in multiple slots. Yet all of the engine's slot operations assume
"one index is only in one slot":

```c
RemoveGreatWork(index)  // stops at the first match, clears only that one, returns that slot's subscript
```

The result is **clearing the wrong slot when moving a Great Work** — what gets cleared is the Great Work with the same index in another slot.
CE's equivalent implementation has the same problem.

### Solution: detect occupancy -> create a new entry

```
1. Call FindOrAddGreatWork(type) normally to get index i
2. If i is already occupied by [any city of any player]:
     a. Call FindOrAddGreatWork with a [fake type] that does not exist in the list
        — the lookup necessarily misses, and the engine goes through its own DoInsertValue append path
     b. Re-fetch the new entry address (it may have moved due to reallocation), change the type to the target type
        and return the new subscript j
     c. If it cannot be created, return -1 and [never fall back] to the occupied old index
3. SetGreatWorkPlayer(j, playerId)
```

This fully reuses the engine's append logic (memory management, field initialization, capacity growth are all correct), with no
need to hand-construct a `GreatWork`, no need to call `DoInsertValue`, and no need to hook any function.

### The duplicate-check scope must be global, not the local city

Early versions used `IndexInUse(buildings, i)` to scan only the target city's slots, on the grounds that
"`RemoveGreatWork` also scans only within a single `Buildings`, so uniqueness within the building is enough".
That reasoning is **wrong** — it missed two things:

| Omission | Consequence |
| --- | --- |
| the `greatWorks` list is at `Game::Culture` level and globally unique | the same index appearing in two cities = the same Great Work duplicated into two copies |
| the entry's `player` field is also single | when adding **across players**, `SetGreatWorkPlayer` rewrites the previous copy's owner |

Concrete reproduction: player 1, city A adds type 42 -> index 0 (owner=1);
player 2, city B adds type 42 again -> `FindOrAddGreatWork` still returns 0 (hit by type),
and the local-city check is false again -> reuse 0 -> the owner is changed to 2. City A's Great Work has been "transferred".

So it now uses `IndexInUseAnywhere(i)`, iterating all cities of all players:

```c
for (int playerId = 0; playerId < kMaxPlayerScan; ++playerId) {
    void* player = g_api->GetPlayer(playerId);
    if (!player) continue;
    // player -> Cities list -> each City's Buildings -> slots
    // hit -> return true
}
```

> Semantic boundary: "moving an existing Great Work" **does not go through** here — `MOVE_GREAT_WORK` / moving the capital /
> relocating buildings go through the engine's own `Process` / `RelocateBuildingGreatWorks`, which must keep using the
> original index (it is the same Great Work changing position in the first place). This function only serves "adding".

### ⚠️ Two dead ends (they cause crashes, do not try)

**1. Temporarily change an existing entry's `greatWorkType` to `-1` to force the lookup to miss**

```
Crashes in CultureAi::DoOffTurn:
  mov rax, [rdx+rdi*8]   ; rdi = 0xFFFFFFFF
  mov rax, [rax+0B8h]    ; rax = 0 -> accessing 0xB8 -> ACCESS_VIOLATION
```

An entry with `type == -1` exists in the list, the AI uses it to look up `Definition::GreatWork` and gets `nullptr`,
then dereferences `+0xB8` directly. Moreover this `-1` gets **persisted into the save file**, and the crash continues after loading.

**2. Temporarily move the vector's `end` pointer forward by one so the lookup cannot see the last entry**

The `pos` that `DoInsertValue` receives happens to equal the (forward-moved) `end`, so it takes the "construct in place" path and
**directly overwrites the original last entry**, losing data.

## Memory layout (verified by disassembly)

```
Player::Instance +0x6D0 = Player::Cities
Player::Cities   +0xD0  = head of the capital city list; node[0]=City*, node[2]=next
City::Instance   +0xCB0 = the Buildings object itself (+0xCA0 is its FAutoVariable wrapper)

Culture +0x118 = greatWorks vector.begin
        +0x120 = vector.end
        +0x128 = vector.capacity
        (element is 24 bytes: +0 greatWorkType, +4 player, +8 turn, +16 name)
```

## Targets

| Target | Method | AOB |
| --- | --- | --- |
| `Game::Culture::Edit` | `HookName` | **none** (47 look-alikes) |
| `Culture::FindOrAddGreatWork` | `HookName` + AOB | 22 bytes, unique |
| `Culture::SetGreatWorkPlayer` | `HookName` + AOB | 19 bytes, unique |
| `Buildings::CanAddGreatWork` | `HookName` + AOB | 38 bytes, unique |
| `Buildings::AddGreatWork` | `HookName` + AOB | 31 bytes, unique |
| `Buildings::AddGreatWorkToSlot` | `HookName` + AOB | 22 bytes, unique (resolution and readiness check only, not used for placement) |
| `Player::Districts_Units_Cities::Find` | `HookName` + AOB | 22 bytes, unique |

> The AOB byte counts are governed by the actual strings of `HookName(sym, "...")` in `GreatWorkAPI.cpp`;
> sync this table when changing the code, do not fill it in from memory.

## Registration environment

Registered only into the **gameplay** script environment — these two interfaces operate on engine gameplay domain objects
(`Culture` / `Buildings`), which the UI script environment does not use and should not expose.

## Log

```
[GreatWorkAPI] symbols ok; GreatWorkManager.AddGreatWork/AddGreatWorkToCity registered
[GreatWorkAPI] AddGreatWorkToCity ok: player=0 city=65536 gw=0 list=0
[GreatWorkAPI] duplicate great work: gw=0 reuse=0 -> new=5
```

The appearance of `duplicate great work` means the multiple-copy support was triggered: `reuse` is the index that would originally have been reused,
`new` is the newly allocated unique index.
