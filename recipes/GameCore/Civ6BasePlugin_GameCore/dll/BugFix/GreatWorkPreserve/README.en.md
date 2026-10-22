# GreatWorkPreserve

> English | [中文](README.md)

> Category: **BugFix** · Back to [Project overview](../../../README.md)

Fix “when the number of extra Great Work slots changes, all Great Works already placed in that building are lost”.

## Defect

When the building is **already built**, `Buildings::ChangeExtraGreatWorkSlots` calls two functions as a pair:

```c
RemoveGreatWorkSlots(this, buildType);   // delete the building's Great Work record, the slot array is freed along with it
AddGreatWorkSlots(this, buildType);      // rebuild 【empty slots】 only from the definition
```

`AddGreatWorkSlots` never reads the old contents, so every rebuilt `greatWorkIndex` is `-1`.
**No data is passed between the two calls** — so all Great Works placed in that building are lost, and are not returned to the player.

**Trigger scenario**: the `AdjustExtraGreatWorkSlots` effect changes the number of Great Work slots of a building
(policy card / wonder / Great Person ability).

## Fix: single hook + private staging

Only hook `ChangeExtraGreatWorkSlots` (a **public** member, RVA `0x109C50`):

```
Hook ChangeExtraGreatWorkSlots
  ├─ changeNum == 0 → pass through as is (the engine will not rebuild anything)
  ├─ entry: CapturePendingSlots(...)        store the valid slot contents into private staging
  ├─ call the original: internally Remove → Add, rebuilding all-empty slots
  └─ exit: RestorePendingSlots(...)         fill the staged contents back into the empty slots by slot position
```

The staging is a plugin-private `std::vector` and holds no game heap pointers. The capture happens **before** the rebuild,
so it does not depend on coincidences such as heap reuse; it is cleared immediately after the exit, so no residue survives across calls.

### Why only one hook

`ChangeExtraGreatWorkSlots` **has only one exit** (the function body has no early `return`),
so a single “capture at entry + fill at exit” covers it completely.

Earlier versions hooked the two private functions it calls internally (`Add`/`RemoveGreatWorkSlots`),
but those two have other callers and would be hit by mistake:

| Caller | Needs save/restore | Consequence of the double hook |
| --- | --- | --- |
| `ChangeExtraGreatWorkSlots` | ✅ yes | normal |
| `RemoveBuilding` / `RemoveAllBuildings` | ❌ the building is demolished, no `Add` follows | staging stays forever, waiting for the next `Add` of the same `buildType` to consume it by mistake |
| `SetBuildingLocation` | ❌ a new building is created, there are no old Great Works anyway | the “restore” is superfluous |

After switching to a single hook none of these problems exist, and it hooks a public symbol, which is more stable than private functions of the same family.

## Three red lines (pitfalls hit before, do not change)

1. **Never change the structure of the `greatWorkBuildings` array** — no dedup, no merge, no moving elements, no changing `count`.
   An earlier version used `memcpy` to move the tail entry into the middle and then `--count`; as a result, writes that locate by array index
   during `MOVE_GREAT_WORK` were all misaligned, showing up as “after moving a Great Work, other Great Works' positions get scrambled”.
2. **Write only `greatWorkIndex`**; do not touch `slotType`, the array pointer, or `count`.
3. **Backfill only fills empty slots** (`greatWorkIndex == -1`) and does not overwrite existing contents — even when interleaved with `MOVE_GREAT_WORK`,
   it will not wash away the Great Work the player just moved.

## Two-round backfill strategy

| Round | Means |
| --- | --- |
| Round 1 · in-place return | the original absolute index still exists, `slotType` matches, and that slot is empty → put it back at the original position (most precise; coexistence of the same category does not get crossed) |
| Round 2 · compatible empty-slot spill | for those not put back in round 1, fill them into the remaining empty slots in order by `slotType`; each item is used only once, each slot is filled only once |

Data is truly lost only when **round 2 also cannot fit it** — that means capacity really is insufficient, and a
`LOST gwIndex=... slotType=...` line is printed per item.

`slotType` is the **slot category** (ART / WRITING / RELIC…), not the Great Work type — a writing slot can hold any writing,
and a palace slot can also hold a writing. Therefore, with multiple slots of the same category, they must be distinguished by “index” or “ordinal within the same category”, not by type alone.

### Why pairing by “ordinal within the same category” alone does not work

An earlier version required “the staged item is the N-th of its category in the old record” to equal “the new empty slot is the N-th in that category”.
Once the slot shape changes, the ordinals shift:

```
shrink: old [-1, 42]  →  new [-1]
      42 is No. 1 (0-based), the new empty slot is No. 0 → ordinals do not match → discarded outright
```

Yet the remaining capacity is actually **enough** to hold 42. This is “silently dropping data when there is space” — worse than insufficient capacity,
because the player cannot see the reason.

The current approach is: return in place first, then look for an empty slot of the same category for those that cannot go back, and report LOST only when there really is none.

## Struct layout (verified by decompilation)

```
Buildings (+0x170)
  +0x0C8  GreatWorkInfo*  greatWorkBuildings   // element 0x20 bytes
  +0x0D8  int             greatWorkBuildingsCount

GreatWorkInfo (0x20)
  +0x00 int              BuildingType
  +0x08 GreatWorkSlot*   GreatWorks
  +0x10 int              GreatWorkSlotCount
  +0x18 int              GreatWorkNum

GreatWorkSlot (0x08)
  +0x00 GreatWorkSlotTypes  slotType
  +0x04 GreatWorkListIndex  greatWorkIndex   // -1 = empty
```

## Targets

| Target | Symbol | Notes |
| --- | --- | --- |
| `Buildings::ChangeExtraGreatWorkSlots` | `HookName` (demangled) + AOB | public, RVA `0x109C50`, the 20-byte prologue has a unique hit |

Signature: `void ChangeExtraGreatWorkSlots(Buildings*, BuildingTypes, GreatWorkSlotTypes, int changeNum)`.

## Verification

First place a few Great Works in some building's Great Work slots, then trigger a slot-count change (a policy card / wonder with
`AdjustExtraGreatWorkSlots`); the Great Works should be preserved, and the log shows:

```
[GreatWorkPreserve] bt=1 in-place=2 filled=1 restored=3/3
```

- `in-place`: number put back in place
- `filled`: number filled into other empty slots by the round-2 spill
- `restored/total`: final preserved / total to restore

When capacity really is insufficient, it additionally prints per item:

```
[GreatWorkPreserve] bt=1 LOST gwIndex=42 slotType=7 (no compatible empty slot)
```

Expected result for the shrink scenario (old `[-1,42]` → new `[-1]`): `restored=1/1`, and 42 lands at index 0.
