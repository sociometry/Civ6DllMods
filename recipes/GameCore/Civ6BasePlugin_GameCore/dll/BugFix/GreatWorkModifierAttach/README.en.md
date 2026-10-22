# GreatWorkModifierAttach

> English | [中文](README.md)

> Category: **BugFix** · Back to [Project overview](../../../README.md)

Make Great Works produced by Great Persons trigger the Modifiers attached to them.

## The problem

Great Works created by Great Persons (Great Writer / Great Artist / Great Musician / Great Archaeologist)
do not trigger the Modifier configured in the Database for that GreatWork;
whereas the very same Great Work coming out of a Product (the Great Merchant's random product) works fine.

## Locating it

The single entry point through which a Great Work gets a Modifier attached is this callback on the GameEffects side:

```c
GameEffects::GameEventHandler::OnGreatWorkAdded_AttachGameEffects(
    GameCore::City::Instance* city, GameCore::GreatWorkTypes type);
```

It is subscribed to an internal GameCore signal in the `GameEffects::GameEventHandler` constructor:

```asm
lea r8, OnGreatWorkAdded_AttachGameEffects
lea rcx, [r14+19B0h]          ; Game::Signals + 0x19B0
call ProtoSignal<...>::Subscribe
lea r8, OnGreatWorkRemoved_DetachGameEffects
lea rcx, [r14+19C8h]          ; Game::Signals + 0x19C8
```

So “whoever fires the `Signals+0x19B0` Dispatch is whoever can attach a Modifier”.

Backing out the offset of each of the **75** `ProtoSignal::Dispatch` call sites in the binary and tallying them:

| Offset | Sender | Scenario |
| --- | --- | --- |
| **`+0x19B0`** | `Operations::Manager::Process` | Moving / swapping Great Works |
| **`+0x19B0`** | `GrantRandomProduct` | Product (Great Merchant) |
| `+0x19C8` | The same two places as above | The corresponding detach |
| `+0x240` | `Buildings::AddGreatWork` | **Another signal**, unrelated to attaching Modifiers |

Conclusion: across the whole binary `+0x19B0` is fired from only **2 places**.

Now the ordinary creation path:

```
ActivateGreatPerson::Start
  └─ Game::Culture::AddGreatWorkToTile
       └─ Buildings::AddGreatWorkToBuilding      ← the only slot-writing exit
```

From start to finish this chain **only calls** `Reporting::Event::Send(GREAT_WORK_CREATED)` —
that is a **reporting event** sent to the UI (used by `GreatWorkShowcase.lua` / `LaunchBar.lua`),
an entirely separate mechanism from GameEffects' Modifiers. The `+0x19B0` that should have been fired is never fired.

The reason Product behaves normally is precisely that `GrantRandomProduct` makes up for that Dispatch itself.

## Approach

### Great Person activation

Hook `Buildings::AddGreatWorkToBuilding` and fire one extra Dispatch after the original function returns successfully (`>= 0`):

```c
int Hook_AddGreatWorkToBuilding(Buildings* b, int individualType,
                                uint64_t plotCoord, int* outBuildType) {
    int added = g_origAddToBuilding(b, individualType, plotCoord, outBuildType);
    if (added >= 0) {
        void* city    = *(void**)b;                              // Buildings + 0x00 = m_pCity
        char* signals = *(char**)((char*)Game::Instance::Edit() + 0x280);
        g_dispatch(signals + 0x19B0, city, added);
    }
    return added;
}
```

### Lua interface

`GreatWorkAPI` calls the exported function after its own placement succeeds:

```c
g_addGreatWork(buildings, listIndex);          // Buildings::AddGreatWork
NotifyGreatWorkAdded(culture, buildings, listIndex);
```

Note that `NotifyGreatWorkAdded` must also translate the index internally:

```c
// AddGreatWork receives a greatWorkListIndex, while the signal wants a greatWorkType.
// One greatWorkType can have several list entries (the 2nd, 3rd... copy of the same kind
// of Great Work), so the Culture list has to be queried back to actually get the type.
int GreatWorkTypeOfListIndex(void* culture, int listIndex) {
    char* entry = GetGreatWorkEntry(culture, listIndex);
    return entry ? *(int*)(entry + OFF_GW_TYPE) : -1;
}
```

If the module is not loaded / symbol resolution fails, `GreatWorkModifierAttach_NotifyAdded` is an empty implementation,
and the interface's main flow is unaffected.

### Covering both paths

The game has two “a new Great Work is born” paths; this module covers each with a different mechanism:

| Path | Mechanism |
| --- | --- |
| Great Person activation: `AddGreatWorkToTile` → `AddGreatWorkToBuilding` | **hook** that function and fire afterwards once it succeeds |
| This project's Lua interface: `GreatWorkManager.AddGreatWork` / `AddGreatWorkToCity` | **call** `GreatWorkModifierAttach_NotifyAdded()` explicitly after a successful placement |

Why the second path exports a function instead of hooking: it goes through `Buildings::AddGreatWork`, and that function
**has 5 callers, not one of which is a “new creation”**:

| Caller | Scenario |
| --- | --- |
| `MoveCapital` / `FindandMovetoNewCapital` | Relocating the capital |
| `RelocateBuildingGreatWorks` | Relocating a building |
| `ReceiveGreatWork` | Receiving a Great Work |
| `Player::Cities::AddGreatWork` | Espionage steal |

Firing Added on those paths would make the new city attach duplicates, while the corresponding Removed signal
(`Signals+0x19C8`) **is fired by nobody in the whole binary**, so the old city's copy can never be detached — the more you move, the more pile up.
So only a caller that genuinely “adds a new one” may notify explicitly.

### Other candidates

| Candidate hook | Verdict | Reason |
| --- | --- | --- |
| `AddGreatWorkToSlot` | Not hooked | Common landing point; Product / Process each already fire it, so hooking it would **attach twice** |

## Pitfall we hit: ICF makes symbol names lie

In the binary the symbol name of this Dispatch is:

```
?Dispatch@?$ProtoSignal@$$A6AXW4PlayerTypes@GameCore@@PEBVInstance@City@2@@Z...
```

It looks like an instance of `void(PlayerTypes, City const*)`, but the register usage at the actual firing site is:

```asm
mov rdx, [rbp+playerID]     ; rdx = City::Instance*
mov r8d, ebx                ; r8d = GreatWorkTypes
call Dispatch
```

Reason: the linker's ICF (identical code folding) merged Dispatch instances whose arguments have **the same shape**
into one single copy of machine code; the symbol name is only the result of “which one got linked first”.

A template's ABI depends only on the registers and the count of the parameters, not on the concrete types:

| Register | Meaning |
| --- | --- |
| `rcx` | `ProtoSignal*` |
| `rdx` | 1st actual argument (here `City::Instance const*`) |
| `r8d` | 2nd actual argument (here `GreatWorkTypes`) |

So declaring and calling it as `(signal, city, greatWorkType)` is enough; no need to agonize over the symbol name.
This was verified by “the first 29 bytes of `Dispatch` being byte-for-byte identical to another instance, diverging only at byte 30”.

## Targets

| Target | RVA | Address resolution | AOB fallback |
| --- | --- | --- | --- |
| `City::Buildings::AddGreatWorkToBuilding` | `0x109890` | Symbol name | None (2 hits for this prologue in the binary) |
| `ProtoSignal<...>::Dispatch` | `0xDA700` | Symbol name | None (ICF; 2 hits for the AOB) |
| `Game::Instance::Edit` | `0x951550` | Symbol name | `48 8B 05 ?? ?? ?? ?? C3` |

> `Game::Instance::Edit` is a single `mov rax, cs:ms_pkCurrentGame; retn`,
> returning the global pointer directly; it does not need a `Game::Instance*` argument.

The module also exports one C function for other modules of this project to use:

| Export | Purpose |
| --- | --- |
| `GreatWorkModifierAttach_NotifyAdded(buildings, greatWorkType)` | Called by `GreatWorkAPI` after a genuine addition |

## Verification

Loader log:

```
[GreatWorkModifierAttach] active: great works from Great Person now fire GreatWorkAdded
```

In game: have a Great Writer create a Writing, and the corresponding bonus should show up in that city's Modifier panel.
If the log turns into `... unresolved - skipped`, symbols drifted after a game update and this module automatically skips itself,
**without affecting other modules**.
