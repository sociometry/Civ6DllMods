# GreatWorkYieldStack

> English | [中文](README.md)

> Category: **BugFix** · Back to [Project Overview](../../../README.md)

Let the yield bonuses that the same kind of Great Work receives from multiple sources stack.

## Problem

The implementation of `City::Culture::GetGreatWorkYieldChange(this, objType, yieldType)` is **return on first hit**:

```c
for (int* p = this->+0x68; p < this->+0x70; p += 0x10) {
    if (p[0] == objType && p[1] == yieldType && p[2] > 0)
        return p[2];        // ← returns on the first hit; every later entry of the same type is ignored
}
return 0;
```

It walks the range `[this+0x68, this+0x70)` with an element stride of **0x10**; the first three ints of each entry are
`{objectType, yieldType, amount}`.

**Consequence**: when the same `(objType, yieldType)` has multiple sources (for example two "Writings" Great Works each giving +3 Culture),
only the first one takes effect and all the rest are ignored.

Its only caller is `City::Buildings::GetBuildingYieldFromGreatWorks` (city Great Work yield settlement).

## Approach

Hook `GetGreatWorkYieldChange` and turn "return on first hit" into **iterate and sum**:

```c
int sum = 0;
for (each {objectType, yieldType, amount})
    if (matches && amount > 0) sum += amount;
return sum;
```

The `amount > 0` filter follows the same rule as the reference implementation, so negative values are not summed in either.

## Differences from the CE reference implementation (intentional)

CE's `Hook_BB` slides an `int*` comparison 4 bytes at a time, relying on the fact that "a sliding window will always sweep across the aligned hit" to achieve the sum.

This port instead walks with the **stride of 16 bytes proven by disassembly**:

| Item | CE approach | This module |
| --- | --- | --- |
| Stepping | 4-byte sliding | **16 bytes** (measured stride) |
| Risk of misaligned false matches | Yes (may match a triple spanning fields) | None |
| Sum semantics | Accumulate after filtering with `amount > 0` | Same |

Semantically equivalent, but free of the risk of mismatching unaligned triples.

## Safety nets

Game updates may shift the layout, so two layers of protection were added:

1. When the `begin` / `end` pointers are invalid or the span is too large (> 4096 entries) → **fall back to the original function**;
2. When there is no trampoline and layout validation fails → **neutrally return 0** (vanilla behavior is unchanged; the bonuses simply still do not stack).

## Targets

| Target | RVA | Prologue |
| --- | --- | --- |
| `City::Culture::GetGreatWorkYieldChange` | — | `48 8B 41 68 4C 8B 49 70` (single AOB hit) |

## Source

Ported from `Main.cpp` of CivilizationVI_CommunityExtension 1.3.0-beta (`GET_BB_OFFSET=0x11C630`
+ `Hook_BB`) to the Civ6DllLoader plugin framework.

## Log

```
[GreatWorkYieldStack] hooked GetGreatWorkYieldChange; great work yields now stack
```
