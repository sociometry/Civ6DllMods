# GreatWorkOptimizer

> English | [中文](README.md)

> Category: **BugFix** · Back to [Project overview](../../../README.md)

Mitigate the stutter of the AI's Great Work assignment computation in the late game.

## Problem

Every turn the AI has to pick, for every building of every city, “which Great Work goes into which slot”. The vanilla implementation:

1. iterate over candidate Great Works × candidate slots and do a full-permutation evaluation;
2. recompute the yield value of the slot combination on every evaluation.

Complexity is about **O(G³)** (G = number of Great Works). In the late game a culture-oriented AI may hold dozens of Great Works,
and this computation eats a whole frame, showing up as **obvious stutter when passing turns**.

## Approach

Three changes, each corresponding to a vanilla hotspot:

| Change | Means |
| --- | --- |
| reuse computation results | add a **result cache** to `CultureAi_GreatWorks::Execute`; repeated calls within the same turn hit it directly |
| pruning | slot combinations are first classified by FNV-1a hash; isomorphic combinations are computed only once |
| clamp the scale | limit the candidate set size inside `bounded_knapsack`, avoiding a combinatorial explosion |

For **human players** this recomputation is meaningless (players decide placements themselves), so when a local human is detected
the whole optimization path is **skipped directly** and the vanilla full recomputation is taken — guaranteeing that human-side behavior is fully identical to vanilla.

## Key implementation

### Human detection without hooking

Judge by calling `Player::Instance::IsHuman` directly, **no hook**. When resolution fails, the human-skip branch
automatically becomes ineffective (falling back to the vanilla full recomputation), and never hurts the AI path.

### Log throttling

The recomputation path is itself a performance hotspot; logging every time would only make it slower. So all logs are reduced to
**every N calls / first call only**.

## Targets

| Target | Method | AOB |
| --- | --- | --- |
| `CultureAi_GreatWorks::Execute` | `HookName` + AOB | 24 bytes, unique |
| `bounded_knapsack` | `HookName` + AOB | 23 bytes, unique |
| `Definitions::GetBuildingDefinitions` | `HookName` + AOB | 20 bytes (contains `??`), unique |
| `Player::Instance::IsHuman` | `HookName` direct call | **none** (7 same-shaped hits) |

## Slot layout (verified by decompilation)

```
Execute:  this   +0x28  = Player::Instance*
          player +0x6D0 = Player::Cities
          cities +0xD0  = city linked-list head (node[0]=City*, node[2]=next)
          city   +0xD78 = GreatWorkInfo array, +0xD88 = capacity, element 0x20 bytes

GreatWorkInfo +0x00 = BuildingType
              +0x08 = int[2] / slot: [0]=GreatWorkListIndex, [1]=theme (-1 = empty slot)

Definition::Building             +0x1C8 / +0x1D0 = Building_GreatWork range
Definition::Building_GreatWork   +0x24 = SlotNum
```

## Source

Ported from `GreatWorkOptimizer.cpp` of CivilizationVI_CommunityExtension 1.3.0-beta to the
Civ6DllLoader plugin framework. The logic proper (FNV-1a hash, Execute cache skip, knapsack clamping)
is copied from the reference implementation; the slot layout has been verified against the current IDB by disassembly.

## Logs

```
[GreatWorkOptimizer] installed 2/2 hooks; buildingDefs=ok humanSkip=on
[GreatWorkOptimizer] player=1 gwCount=0 exec=1 cacheSkip=0 humanSkip=1
```
