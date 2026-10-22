# CustomRequirement

> English | [中文](README.md)

Adds two custom **Requirement**s to Civilization VI: Gathering Storm (sample modules under the `GameplayExtension` category):

| Source | Responsibility | Hooks installed after registration |
|---|---|---|
| [PlayerPropertyMatches.cpp](PlayerPropertyMatches.cpp) | Player property threshold; address resolution, argument parsing, factory, evaluation, weak reference and refresh | PlotProperty OnInitialize / Test, player property Dispatch, Lua SetProperty (4) |
| [PlotRouteTypeMatches.cpp](PlotRouteTypeMatches.cpp) | Route type; address resolution, argument parsing, factory, evaluation, weak reference and refresh | Terrain TestPlot, PlotRequirement Test, SetRouteType (3) |
| [CustomRequirement.h](CustomRequirement.h) | Declarations of the two `*_Init` / `*_Shutdown` | none |

The call site is the plugin entry [dllmain.cpp](../../dllmain.cpp): `OnLoad` calls `CustomRequirement_InitPlayerProperty` /
`CustomRequirement_InitPlotRoute` in order, and `OnUnload` calls the two corresponding `*_Shutdown`. A failure in either module does not affect the other
(each installs its own hooks and writes its own logs).

Each cpp has its own anonymous namespace and state. Helper code such as address resolution, shared pointers and the Registry is deliberately duplicated so that each file can be read and copied on its own; the two modules do not call each other and no longer depend on a centralized implementation file.

## Arguments and current smoke-test SQL

| Requirement | Arguments |
|---|---|
| `REQUIREMENT_PLAYER_PROPERTY_MATCHES` | `PropertyName`, `PropertyMinimum` (signed 32-bit threshold) |
| `REQUIREMENT_PLOT_ROUTE_TYPE_MATCHES` | `RouteType` (for example `ROUTE_ANCIENT_ROAD`) |

The companion SQL is in [Civ6Mod/Data/Gameplay.sql](../../../Civ6Mod/Data/Gameplay.sql) (this project maintains only one copy; the sample data for both the Requirements and the Effects lives there).
The property Modifier is attached to `TRAIT_LEADER_MAJOR_CIV` with thresholds 1 and 2, each adding 1 worker charge; the route Modifier is attached to the farm,
adding 10 Gold when an ancient road matches (the old ID was `MODIFIER_EXAMPLE_ROAD_FOOD` but the actual argument was `YIELD_GOLD`, so it has been renamed to
`MODIFIER_EXAMPLE_ROAD_GOLD`).

## Build and verification

From the repository root:

```powershell
& MSBuild.exe recipes\GameCore\Civ6BasePlugin_GameCore\dll\Civ6BasePlugin_GameCore.vcxproj /t:Rebuild /m /p:Configuration=Release /p:Platform=x64
```

Output: `x64\Release\Civ6BasePlugin_GameCore.dll` (this module is already merged into that plugin and no longer ships as a separate DLL).

## General guide to adding a new Requirement

This chapter summarizes how to add a custom Requirement. The goal is not to copy one particular class but to grasp the connection pattern shared by all Requirements:

```text
SQL definition
  → Requirement registry looks up the factory by type name
  → factory::Create creates the native requirement object
  → OnInitialize reads the arguments and saves the context
  → Test / TestPlot computes the current state
  → SetState notifies the Modifier
  → property, plot or game events trigger a re-test
```

### 1. The five questions about a Requirement

Answer these before writing code:

1. **Who is the subject?** A player, a unit, a city, a plot, or some other object?
2. **Where is the data to evaluate?** Object fields, interfaces, Definitions tables, or a Property container?
3. **When does the first test happen?** During creation, at turn start, or after the first event?
4. **Which events change the result?** Property writes, route construction, terrain changes, city capture, etc.
5. **Can a native object be reused?** Prefer the donor requirement whose layout and lifetime are closest.

The difficulty of a Requirement is usually not the comparison expression, but wiring these five things into the game's existing object, factory and event systems.

### 2. SQL is the entry point for C++

SQL needs at least four sets of records:

```sql
INSERT INTO Requirements (RequirementId, RequirementType)
VALUES ('REQ_EXAMPLE', 'REQUIREMENT_MY_CUSTOM_REQ');

INSERT INTO RequirementArguments (RequirementId, Name, Value)
VALUES ('REQ_EXAMPLE', 'MyArgument', 'SomeValue');

INSERT INTO RequirementSets (RequirementSetId, RequirementSetType)
VALUES ('REQSET_EXAMPLE', 'REQUIREMENTSET_TEST_ALL');

INSERT INTO RequirementSetRequirements (RequirementSetId, RequirementId)
VALUES ('REQSET_EXAMPLE', 'REQ_EXAMPLE');
```

Then attach the requirement set to a Modifier:

```sql
INSERT INTO Modifiers
    (ModifierId, ModifierType, SubjectRequirementSetId)
VALUES
    ('MODIFIER_EXAMPLE', 'MODIFIER_PLAYER_ADJUST_PLOT_YIELD', 'REQSET_EXAMPLE');
```

Notes:

- `RequirementType` must exactly match the factory's `GetTypeName()`;
- `RequirementId` is the instance name, `RequirementType` is the class/factory type name;
- argument names must exactly match the keys parsed by C++;
- smoke-test with a simple Modifier first; do not wire up complex effects from the start;
- no errors in the database log only means SQL was accepted; it does not prove the C++ requirement executed correctly.

### 3. Choosing a donor requirement

Normally you should not construct a C++ requirement object from scratch. Choose a native donor:

| Custom goal | Suitable donor | Reused content |
|---|---|---|
| Player/unit property | `PlotPropertyMatches` or a Property requirement with a similar layout | `SimpleRequirement`, context, shared_ptr lifetime |
| Plot field | `PlotTerrainTypeMatches` | `PlotRequirement`, plot subject, `TestPlot` call chain |
| City values | A native City/Player numeric requirement | City interfaces and the corresponding signal |

When choosing a donor, verify:

- object size and key field offsets;
- the `OnInitialize` argument signature;
- the return value encoding of `Test` or `TestPlot`;
- the donor factory's vtable;
- the subject interface GUID used by the donor;
- how the donor's signal/events refresh.

Knowing only the class name or a single RVA is not enough. You must know its position in the object lifetime.

### 4. Information to record during decompilation

Build an address table for each version of the GameCore DLL, recording at least:

```text
function name / symbol name
current Steam RVA (data/vtable only, or a fallback for name resolution)
function signature
owning object or class
call timing
key field offsets
return value meaning
```

Focus on:

- the vtable slots of factory `GetTypeName`, `Create`, `ValidateDefinition`;
- donor `OnInitialize` and `Test`/`TestPlot`;
- `RequirementDefinition::GetArguments()`;
- ArgumentProcessor's `GetStringValue` or `GetDefinitionIndex`;
- the subject interface GUID and the vtable slot that fetches the object;
- native functions that modify data, such as `SetRouteType`;
- the corresponding signal dispatcher.

An RVA is only address location information. After confirming the RVA is correct, also confirm:

1. the argument count and register conventions are correct;
2. whether the return value is `bool`, a plain integer, or an enum;
3. whether the object is already fully constructed when the function is called;
4. whether it synchronously calls other virtual functions internally.

### 5. The general way to implement a factory

The safest pattern is:

```text
find the donor factory
  → copy the donor factory vtable into your own writable storage
  → replace only GetTypeName / Create / ValidateDefinition
  → append your factory to the registry vector
```

The factory's responsibilities are small:

- `GetTypeName()` returns the new SQL type name;
- `ValidateDefinition()` accepts only the new requirement type hash;
- `Create()` calls the donor's native Create to obtain a complete object and shared_ptr.

Do not use `operator new` inside `Create()` to guess the object size, and do not assemble the `shared_ptr` control block yourself.

In the current project the route requirement uses the Create of `PlotTerrainTypeMatches` but does not replace the requirement instance's vtable. This preserves native:

- object allocation;
- constructors;
- `enable_shared_from_this`;
- signal connections;
- reference counting;
- destruction.

### 6. General principles of argument parsing

Requirement arguments come from `RequirementParameters`; do not treat it as a plain C struct and read it at will.

Recommended flow:

```text
params
  → definition accessor
  → GetArguments()
  → construct a temporary ArgumentProcessor object
  → use the game's own GetStringValue / GetDefinitionIndex
  → convert into the integer or hash kept by the requirement
```

Must note:

- the temporary ArgumentProcessor must use the correct vtable;
- shared pointers in arguments must have their reference count incremented and released;
- if a string is returned, copy it into a local buffer while its ownership is still valid;
- do not misuse a sparse `GetDefinitionIndex` on a mismatched Definitions collection;
- on argument parsing failure, return a safe NotMet or let creation fail; never write guessed values.

The current route implementation first parses:

```text
RouteType = "ROUTE_ANCIENT_ROAD"
```

and then converts it into a route index. The current version also uses RAII to release temporary shared pointers, see `SharedPointerPairGuard` in the source.

### 7. Create timing is where mistakes are most likely

The native factory's `Create()` may be:

```text
allocate object
  → Initialize
  → first Test
  → return shared_ptr
```

So "call the donor Create first, then set fields after it returns" is not necessarily correct. The first Test may already have happened.

If custom fields are not yet visible during Create, use a temporary context, for example:

```cpp
thread_local int pendingIndex = -1;

pendingIndex = parsedValue;
nativeCreate(...);
restore_previous_pendingIndex();
```

The current route implementation uses `PendingRouteIndexScope`, because the first `TestPlot` inside native Create happens before the object is added to the weak registry.

If Creates can nest, you cannot simply hardcode `-1` at the end; you must restore the value from before entry.

### 8. Test, TestPlot and RequirementState

A custom evaluation should do only three things where possible:

1. find the subject;
2. read the current value;
3. return Met or NotMet.

Different layers may use different encodings. For example, in the current route code:

```text
TestPlot:             2 = Met, 3 = NotMet
PlotRequirement::Test: 0 = Met, 1 = NotMet
```

So an explicit conversion is required; you cannot hand all return values straight to `SetState`.

Recommended form:

```text
native/custom TestPlot
  → read object fields
  → return the enum value prescribed by that layer
  → outer Test converts it into RequirementState
  → SetState
```

When encountering a null subject, a missing argument or an invalid index, return NotMet by default; never dereference a null pointer.

### 9. Object lifetime: do not store raw pointers

Requirements can be deleted, rebuilt, or invalidated by a GameCore reload. Storing only raw pointers causes use-after-free.

The current project's general approach exploits the native `enable_shared_from_this` layout:

```text
object + 0x08 → weak object
object + 0x10 → weak control block
```

Save the weak control block at registration time, and when using it:

```text
TryLockWeakReference
  → success increments the strong count
  → use the object
  → ReleaseStrongReference
```

During event refresh you must lock a strong reference before accessing the object; you cannot simply trust that an old raw pointer is still valid while iterating.

### 10. You must refresh proactively after the result changes

Testing once when the Requirement is created does not mean it will re-test automatically later.

You need to find the events that actually change the evaluated data:

| Evaluated data | Common refresh points |
|---|---|
| Player Property | Property dispatcher, Lua `Player:SetProperty` |
| Plot route | `SetRouteType`, narrow route signal |
| City population/amenities | the corresponding city mutation function or city signal |
| Terrain/resource | plot mutation functions, map edit events |

The refresh flow is usually:

```text
call the native mutation function
  → native data has already been written
  → find the affected requirement
  → re-Test
  → call SetState
```

Refresh hooks should prefer narrow, semantically explicit mutation functions. Do not preferentially hook a low-level `Dispatch` shared by all messages, or you will easily run into wrong ABI, recursion and high-frequency overhead.

### 11. Safety boundaries of global hooks

A global hook is not frightening in itself; the key is that it must filter:

```text
if it is a custom requirement → use the custom logic
otherwise                     → call the original function in full
```

You can identify custom objects using:

- the definition type hash;
- the object address in the weak registry;
- an explicit factory/type name.

Never let a custom hook change the behavior of all native requirements.

Especially avoid:

- guessing the instance vtable length and copying the whole vtable;
- overwriting the native object's RTTI/Complete Object Locator;
- hooking a function at the wrong inheritance layer;
- receiving arguments with the wrong member function signature.

### 12. General handling of enum values such as Route index and Terrain index

Definitions-type arguments are usually not hashes but indices into a Definitions collection.

General steps:

```text
SQL name
  → ArgumentProcessor obtains the string
  → GetDefinitions() obtains the vector
  → compare each definition's type name
  → index = iterator - begin
  → write into the object field
```

In the current route requirement:

```text
plot + 0x47 = the plot's actual current route index
requirement + 0xA8 = the expected route index
```

So the `0` corresponding to `ROUTE_ANCIENT_ROAD` is essentially its position in the game's RouteDefinitions table. Hardcoding the vanilla order is, in the current code, a fallback for when the definition-table scan finds nothing; it cannot be the only implementation.

## Current Loader compatibility and reload verification

- `dll/HookAPI.h` is in sync with `common/HookAPI.h` of the current Civ6DllLoader; when upgrading, copy the whole header and rebuild.
- Both Debug and Release define `CIV6_PLUGIN_GAMECORE`, and the header generates the `Civ6PluginKind` export; `OnLoad` checks the ABI first.
- Run the build command above from the repository root. On a machine where VS 2026 only has v145, append `/p:PlatformToolset=v145`; the project keeps v143 by default.
- Place the artifact in the enabled mod's `Binaries/Win64/Plugins/GameCore/`, and test XP2 with the Loader from this update.
- Within the same game process, repeatedly run "enter match → trigger the feature → save → return to main menu → load save → trigger the feature again".
- Check `Civ6DllLoader_Loader.log` for plugin load, unload queueing, release after engine unload, reload, and this plugin's success logs; a successful compile is no substitute for in-game verification.
- `OnUnload` releases the native weak references held by the two kinds of Requirement registry while GameCore is still mapped; refresh snapshots hold weak references independently and refuse new tracking after cleanup.
- factory/vtable are left for the engine's later teardown; the plugin DLL is released by the Loader after the GameCore unload has completed.
- Verify that the Requirement refreshes when the player property crosses the SQL threshold, and that plot yields change after building/changing a route; run it again after loading a save.
