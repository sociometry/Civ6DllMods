#include "pch.h"
#include "NewRequirements.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <new>

// REQUIREMENT_PLOT_ROUTE_TYPE_MATCHES
// Self-contained teaching example: addresses, state, factory, parsing and hooks
// all live here. Helpers are intentionally duplicated in the other REQ file.
// Registration and pre-unload cleanup are exposed to dllmain.cpp.
// Keep the native PlotTerrain instance vtable and lifetime. Never hook its OnInitialize.
namespace {

constexpr int kOffActive = 0x1C;
constexpr int kOffModifierContext = 0x40;
constexpr int kFactoryGetTypeNameSlot = 1;
constexpr int kFactoryCreateSlot = 3;
constexpr int kFactoryValidateDefinitionSlot = 5;
constexpr int kRouteTypePlotOffset = 0x47;
constexpr int kRouteTypeRequirementOffset = 0xA8;

struct SymbolSpec {
    const char* name;        // demangled symbol name; empty for data-only symbols
    uintptr_t currentRva;    // current Steam GameCore coordinate, used only as fallback
};

const SymbolSpec kSymGetTypes{"GameEffects::IRequirementFactory>::GetTypes", 0x626740};
const SymbolSpec kSymSetState{"GameEffects::Requirements::SimpleRequirement::SetState", 0x9755E0};
const SymbolSpec kSymRouteTestPlot{"GameEffects::Requirements::PlotTerrainTypeMatches::TestPlot", 0x806750};
const SymbolSpec kSymPlotRequirementTest{"GameEffects::Requirements::PlotRequirement::Test", 0x7EDDA0};
const SymbolSpec kSymSetRouteTypeOnPlot{"GameCore::Map::Route::Builder::SetRouteType_1", 0x361670};
const SymbolSpec kSymRouteGetDefinitions{"GameCore::Definitions::GetRouteDefinitions", 0x7D1760};
const SymbolSpec kSymIPlotInterfaceGuid{"", 0xA9A508};
const SymbolSpec kSymMakeHash{"GameCore::Utilities::MakeHash", 0x606270};
const SymbolSpec kSymOpNew{"operator_new", 0x990900};
const SymbolSpec kSymArgGetStringValue{"GameEffects::ArgumentProcessor::GetStringValue", 0x9565C0};
const SymbolSpec kSymSharedPtrAssign{"GameEffects::IModifierDefinition>::operator=", 0x6D8DE0};
const SymbolSpec kSymArgProcessorVftable{"", 0xAA39E0};

HookAPI* g_api = nullptr;

using GetTypesFn = void*(__cdecl*)();
using OpNewFn = void*(__cdecl*)(size_t);
using MakeHashFn = uint32_t(__cdecl*)(const char*);
using SetStateFn = void(__fastcall*)(void* req, int state, char notify);
using TestFn = int(__fastcall*)(void* req, void* a2, void* a3);
using RouteFactoryCreateFn = void*(__fastcall*)(void* self, void* outSharedPtr, const void* params);
using RouteGetDefinitionsFn = void*(__cdecl*)();
using RouteTestPlotFn = int(__fastcall*)(void* req, void* plot);
using PlotRequirementTestFn = int(__fastcall*)(void* req, void* a2, void* a3);
using SetRouteTypeOnPlotFn = void(__fastcall*)(void* builder, void* plot, unsigned int routeType, unsigned int player);
GetTypesFn g_getTypes = nullptr;
OpNewFn g_opNew = nullptr;
MakeHashFn g_makeHash = nullptr;
SetStateFn g_setState = nullptr;
void* g_argProcessorVftable = nullptr;
void* g_routeTestPlot = nullptr;
void* g_plotRequirementTest = nullptr;
void* g_setRouteTypeOnPlot = nullptr;
void* g_routeGetDefinitions = nullptr;
RouteFactoryCreateFn g_createRouteFactory = nullptr;
RouteTestPlotFn g_origRouteTestPlot = nullptr;
PlotRequirementTestFn g_origPlotRequirementTest = nullptr;
SetRouteTypeOnPlotFn g_origSetRouteTypeOnPlot = nullptr;
uint32_t g_routeRequirementTypeHash = 0;

// Preserve the donor factory's RTTI entry at vtable[-1].
thread_local int g_pendingRouteIndex = -1;
alignas(16) uint8_t g_iPlotInterfaceGuid[16]{};
void* g_ourRouteFactoryVftableStorage[7]{};
void** const g_ourRouteFactoryVftable = g_ourRouteFactoryVftableStorage + 1;
bool g_routeVtableReady = false;
int __fastcall Hook_RouteTestPlot(void* req, void* plot);

struct WeakRequirementRef {
    void* object;
    void* control;
    int routeIndex;
};

constexpr size_t kMaxWeakRequirements = 4096;
WeakRequirementRef g_weakRequirements[kMaxWeakRequirements]{};
size_t g_weakRequirementCount = 0;
bool g_weakRegistryStopping = false;
SRWLOCK g_weakRequirementLock = SRWLOCK_INIT;

struct RequirementFactoryObject { void** vftable; };
struct StdVectorPtr {
    void** begin;
    void** end;
    void** capacity;
};
bool FindFactoryObjectByTypeName(const char* wantedName, RequirementFactoryObject** outFactory);

// OnLoad requires the current HookAPI layout before any resolver is accessed.
void* ResolveSpec(HookAPI* api, const SymbolSpec& spec) {
    if (!api) {
        return nullptr;
    }

    // Function symbols are resolved by name first.  The runtime owns the
    // old->current pdata mapping, so this plugin does not carry old RVAs.
    if (spec.name && spec.name[0] && api->ResolveByName) {
        if (void* addr = api->ResolveByName(kHookImageGameCoreXP2, spec.name)) {
            return addr;
        }
    }

    // Data/vtable symbols (and a name fallback) use the current Steam image
    // coordinate. ResolveByRvaEx deliberately bypasses pdata correction.
    if (spec.currentRva && api->ResolveByRvaEx) {
        return api->ResolveByRvaEx(
            kHookImageGameCoreXP2,
            spec.currentRva,
            1,
            kSectionImage);
    }
    return nullptr;
}

bool LoadGuidFromImage(HookAPI* api, const SymbolSpec& spec, uint8_t* out16) {
    void* addr = ResolveSpec(api, spec);
    if (!addr) {
        return false;
    }
    std::memcpy(out16, addr, 16);
    return true;
}


void* QueryInterface(void* modifierObject, const void* guid, bool required) {
    if (!modifierObject) {
        return nullptr;
    }
    auto** vt = *reinterpret_cast<void***>(modifierObject);
    using QiFn = void*(__fastcall*)(void*, const void*, bool);
    return reinterpret_cast<QiFn>(vt[2])(modifierObject, guid, required);
}


uint32_t GetDefinitionTypeHash(void* definition) {
    if (!definition) {
        return 0;
    }
    auto** vt = *reinterpret_cast<void***>(definition);
    if (!vt || !vt[2]) {
        return 0;
    }
    using GetTypeHashFn = uint32_t(__fastcall*)(void*);
    return reinterpret_cast<GetTypeHashFn>(vt[2])(definition);
}


void InvokeSetState(void* req, int state) {
    if (g_setState) {
        g_setState(req, state, 1);
    }
}


void ReleaseStrongReference(void* control) {
    if (!control) {
        return;
    }
    auto* useCount = reinterpret_cast<volatile LONG*>(static_cast<char*>(control) + 8);
    if (_InterlockedDecrement(useCount) != 0) {
        return;
    }
    auto** vt = *reinterpret_cast<void***>(control);
    using RefCountFn = void(__fastcall*)(void*);
    reinterpret_cast<RefCountFn>(vt[0])(control);  // _Destroy
    auto* weakCount = reinterpret_cast<volatile LONG*>(static_cast<char*>(control) + 12);
    if (_InterlockedDecrement(weakCount) == 0) {
        reinterpret_cast<RefCountFn>(vt[1])(control);  // _Delete_this
    }
}


void ReleaseSharedPointerPair(void* pair) {
    if (!pair) {
        return;
    }
    void* control = *reinterpret_cast<void**>(static_cast<char*>(pair) + 8);
    if (!control) {
        return;
    }
    auto* useCount = reinterpret_cast<volatile LONG*>(static_cast<char*>(control) + 8);
    if (_InterlockedDecrement(useCount) == 0) {
        auto** vt = *reinterpret_cast<void***>(control);
        using RefCountFn = void(__fastcall*)(void*);
        reinterpret_cast<RefCountFn>(vt[0])(control);  // _Destroy
        auto* weakCount = reinterpret_cast<volatile LONG*>(static_cast<char*>(control) + 12);
        if (_InterlockedDecrement(weakCount) == 0) {
            reinterpret_cast<RefCountFn>(vt[1])(control);  // _Delete_this
        }
    }
    *reinterpret_cast<void**>(pair) = nullptr;
    *reinterpret_cast<void**>(static_cast<char*>(pair) + 8) = nullptr;
}

struct SharedPointerPairGuard {
    void* pair;

    ~SharedPointerPairGuard() {
        ReleaseSharedPointerPair(pair);
    }
};

void ReleaseWeakReference(void* control) {
    if (!control) return;
    auto* weakCount = reinterpret_cast<volatile LONG*>(static_cast<char*>(control) + 12);
    if (_InterlockedDecrement(weakCount) == 0) {
        auto** vt = *reinterpret_cast<void***>(control);
        using DeleteControlFn = void(__fastcall*)(void*);
        reinterpret_cast<DeleteControlFn>(vt[1])(control);
    }
}

// A copied registry entry must own a weak reference too: OnUnload may clear
// the registry while a refresh still holds its snapshot.
struct WeakRequirementSnapshot {
    WeakRequirementRef entries[kMaxWeakRequirements]{};
    size_t count = 0;

    WeakRequirementSnapshot() {
        AcquireSRWLockShared(&g_weakRequirementLock);
        count = g_weakRequirementCount;
        std::memcpy(entries, g_weakRequirements, count * sizeof(WeakRequirementRef));
        for (size_t i = 0; i < count; ++i) {
            _InterlockedIncrement(reinterpret_cast<volatile LONG*>(
                static_cast<char*>(entries[i].control) + 12));
        }
        ReleaseSRWLockShared(&g_weakRequirementLock);
    }
    ~WeakRequirementSnapshot() {
        for (size_t i = 0; i < count; ++i) ReleaseWeakReference(entries[i].control);
    }
    WeakRequirementSnapshot(const WeakRequirementSnapshot&) = delete;
    WeakRequirementSnapshot& operator=(const WeakRequirementSnapshot&) = delete;
};

void ClearWeakRequirements() {
    WeakRequirementRef retired[kMaxWeakRequirements]{};
    AcquireSRWLockExclusive(&g_weakRequirementLock);
    g_weakRegistryStopping = true;
    const size_t count = g_weakRequirementCount;
    std::memcpy(retired, g_weakRequirements, count * sizeof(WeakRequirementRef));
    std::memset(g_weakRequirements, 0, count * sizeof(WeakRequirementRef));
    g_weakRequirementCount = 0;
    ReleaseSRWLockExclusive(&g_weakRequirementLock);

    // Native control-block destructors still belong to the mapped GameCore.
    // Release outside the registry lock, in OnUnload, never in DllMain.
    for (size_t i = 0; i < count; ++i) ReleaseWeakReference(retired[i].control);
    if (g_api) g_api->Log("PlotRouteTypeMatches: released %llu tracked weak reference(s)",
        static_cast<unsigned long long>(count));
}

void* TryLockWeakReference(const WeakRequirementRef& weak) {
    if (!weak.object || !weak.control) {
        return nullptr;
    }
    auto* useCount = reinterpret_cast<volatile LONG*>(static_cast<char*>(weak.control) + 8);
    LONG current = *useCount;
    while (current != 0) {
        const LONG observed = _InterlockedCompareExchange(useCount, current + 1, current);
        if (observed == current) {
            return weak.object;
        }
        current = observed;
    }
    return nullptr;
}

bool TrackWeakRequirement(void* req, int routeIndex) {
    if (!req) {
        return false;
    }
    // MSVC enable_shared_from_this weak_this occupies req+8: object/control.
    WeakRequirementRef weak{
        *reinterpret_cast<void**>(static_cast<char*>(req) + 8),
        *reinterpret_cast<void**>(static_cast<char*>(req) + 16),
        routeIndex};
    if (!weak.object || !weak.control) {
        if (g_api) {
            g_api->Log(
                "PlotRouteTypeMatches: weak_this unavailable req=%p object=%p control=%p",
                req, weak.object, weak.control);
        }
        return false;
    }

    AcquireSRWLockExclusive(&g_weakRequirementLock);
    if (g_weakRegistryStopping) {
        ReleaseSRWLockExclusive(&g_weakRequirementLock);
        return false;
    }
    for (size_t i = 0; i < g_weakRequirementCount; ++i) {
        if (g_weakRequirements[i].object == weak.object
            && g_weakRequirements[i].control == weak.control) {
            g_weakRequirements[i].routeIndex = routeIndex;
            ReleaseSRWLockExclusive(&g_weakRequirementLock);
            return true;
        }
    }
    if (g_weakRequirementCount >= kMaxWeakRequirements) {
        ReleaseSRWLockExclusive(&g_weakRequirementLock);
        if (g_api) {
            g_api->Log("PlotRouteTypeMatches: weak registry full");
        }
        return false;
    }
    _InterlockedIncrement(
        reinterpret_cast<volatile LONG*>(static_cast<char*>(weak.control) + 12));
    g_weakRequirements[g_weakRequirementCount++] = weak;
    const size_t count = g_weakRequirementCount;
    ReleaseSRWLockExclusive(&g_weakRequirementLock);
    if (g_api) {
        g_api->Log(
            "PlotRouteTypeMatches: weak tracked req=%p control=%p routeIndex=%d count=%llu",
            weak.object,
            weak.control,
            routeIndex,
            static_cast<unsigned long long>(count));
    }
    return true;
}

void* GetPlotFromIPlotInterface(void* qiResult) {
    if (!qiResult || qiResult == reinterpret_cast<void*>(8)) {
        return nullptr;
    }
    // QueryInterface returns the interface payload (+8).  PlotPropertyMatches /
    // AdjustPlotProperty resolve the plot through vtable slot 0 on the iface.
    void* iface = static_cast<char*>(qiResult) - 8;
    auto** vt = *reinterpret_cast<void***>(iface);
    if (!vt || !vt[0]) {
        return nullptr;
    }
    using GetPlotFn = void*(__fastcall*)(void*);
    return reinterpret_cast<GetPlotFn>(vt[0])(iface);
}


void* GetTrackedPlot(void* req) {
    void* ctx = *reinterpret_cast<void**>(static_cast<char*>(req) + kOffModifierContext);
    if (!ctx) {
        return nullptr;
    }
    void* qi = QueryInterface(ctx, g_iPlotInterfaceGuid, true);
    return GetPlotFromIPlotInterface(qi);
}


bool IsRouteRequirement(void* req) {
    if (!req || !g_routeRequirementTypeHash) {
        return false;
    }
    void* definition = *reinterpret_cast<void**>(static_cast<char*>(req) + 0x20);
    return GetDefinitionTypeHash(definition) == g_routeRequirementTypeHash;
}


int FindTrackedRouteIndex(void* req) {
    if (!req) {
        return -1;
    }
    AcquireSRWLockShared(&g_weakRequirementLock);
    for (size_t i = 0; i < g_weakRequirementCount; ++i) {
        // TestPlot `this` is the managed object; tracking stores weak_this.object.
        if (g_weakRequirements[i].object == req) {
            const int index = g_weakRequirements[i].routeIndex;
            ReleaseSRWLockShared(&g_weakRequirementLock);
            return index;
        }
    }
    ReleaseSRWLockShared(&g_weakRequirementLock);
    return -1;
}


int NormalizeRequirementStateFromTestPlot(int testPlotState) {
    // PlotRequirement::Test (+144==0) and PlotPropertyMatches::Test use
    // 0 = Met, 1 = NotMet.  TestPlot itself uses 2 = Met, 3 = NotMet.
    if (testPlotState == 2) {
        return 0;
    }
    if (testPlotState == 3) {
        return 1;
    }
    return testPlotState;
}


int InvokeRouteRequirementTest(void* req) {
    void* plot = GetTrackedPlot(req);
    if (!plot) {
        return 1;  // NotMet
    }
    return NormalizeRequirementStateFromTestPlot(Hook_RouteTestPlot(req, plot));
}


void RefreshTrackedRouteRequirements() {
    WeakRequirementSnapshot snapshot;
    const size_t count = snapshot.count;

    if (g_api) {
        g_api->Log(
            "PlotRouteTypeMatches: route refresh tracked=%llu",
            static_cast<unsigned long long>(count));
    }
    for (size_t i = 0; i < count; ++i) {
        void* req = TryLockWeakReference(snapshot.entries[i]);
        if (!req) {
            continue;
        }
        if (*reinterpret_cast<uint8_t*>(static_cast<char*>(req) + kOffActive)) {
            const int state = InvokeRouteRequirementTest(req);
            InvokeSetState(req, state);
            if (g_api) {
                g_api->Log(
                    "PlotRouteTypeMatches: route refresh req=%p state=%d routeIndex=%d",
                    req,
                    state,
                    snapshot.entries[i].routeIndex);
            }
        }
        ReleaseStrongReference(snapshot.entries[i].control);
    }
}


void __fastcall Hook_SetRouteTypeOnPlot(
    void* builder, void* plot, unsigned int routeType, unsigned int player) {
    if (g_origSetRouteTypeOnPlot) {
        g_origSetRouteTypeOnPlot(builder, plot, routeType, player);
    }
    if (g_api) {
        g_api->Log(
            "PlotRouteTypeMatches: SetRouteType plot=%p route=%u player=%u — refresh",
            plot,
            routeType,
            player);
    }
    RefreshTrackedRouteRequirements();
}


const char* RouteDefinitionNameSEH(void* entry) {
    if (!entry) {
        return nullptr;
    }
    // Isolated SEH: MSVC forbids __try in functions that need C++ unwinding.
    __try {
        const char* raw = *reinterpret_cast<const char* const*>(
            static_cast<char*>(entry) + 0x40);
        if (raw && raw[0]) {
            return raw;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    __try {
        // MSVC std::string at +0x40 (heap vs SSO).
        auto* s = static_cast<char*>(entry) + 0x40;
        const size_t cap = *reinterpret_cast<size_t*>(s + 24);
        const char* p = (cap >= 0x10) ? *reinterpret_cast<const char* const*>(s) : s;
        if (p && p[0]) {
            return p;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}


int RouteTypeIndexFromName(const char* nameStr) {
    if (!nameStr || !nameStr[0]) {
        return -1;
    }
    // The plot stores the index into this live definitions vector at +0x47.
    // Prefer it over a hardcoded XML order so mod-added/reordered routes use the
    // same enum value as the game itself.
    if (g_routeGetDefinitions) {
        auto* defs = reinterpret_cast<void**>(
            reinterpret_cast<RouteGetDefinitionsFn>(g_routeGetDefinitions)());
        if (defs && defs[0] && defs[1]) {
            void** begin = reinterpret_cast<void**>(defs[0]);
            void** end = reinterpret_cast<void**>(defs[1]);
            const ptrdiff_t count = end - begin;
            if (count > 0 && count <= 4096) {
                for (void** it = begin; it != end; ++it) {
                    void* entry = *it;
                    const char* entryName = RouteDefinitionNameSEH(entry);
                    if (entryName && std::strcmp(entryName, nameStr) == 0) {
                        const ptrdiff_t index = it - begin;
                        // Plot::Instance stores route type in a signed byte.
                        if (index <= 127) {
                            return static_cast<int>(index);
                        }
                        return -1;
                    }
                }
            } else if (g_api) {
                g_api->Log(
                    "PlotRouteTypeMatches: route defs span suspicious count=%lld",
                    static_cast<long long>(count));
            }
        }
    }

    // Fallback for the four base-game routes if the definitions accessor is not
    // available during very early database initialization.
    static const char* kVanilla[] = {
        "ROUTE_ANCIENT_ROAD",
        "ROUTE_MEDIEVAL_ROAD",
        "ROUTE_INDUSTRIAL_ROAD",
        "ROUTE_MODERN_ROAD",
    };
    for (int i = 0; i < 4; ++i) {
        if (std::strcmp(nameStr, kVanilla[i]) == 0) {
            return i;
        }
    }
    return -1;
}


int ResolveRouteTypeIndex(int64_t* params) {
    // Same GetStringValue path as PropertyName, then name → RouteTypes index.
    if (!params || !g_argProcessorVftable) {
        return -1;
    }

    using GetArgsFn = void*(__fastcall*)(void* defAccessor);
    using GetStringValueFn = int64_t(__fastcall*)(void* proc, void* outStr, void* keyDesc, void* fallback);
    using AssignFn = void(__fastcall*)(void* dst, void* src);

    auto* getStringValue =
        reinterpret_cast<GetStringValueFn>(ResolveSpec(g_api, kSymArgGetStringValue));
    auto* sharedAssign = reinterpret_cast<AssignFn>(ResolveSpec(g_api, kSymSharedPtrAssign));
    if (!getStringValue || !sharedAssign) {
        return -1;
    }

    void* defAccessor = reinterpret_cast<void*>(params[1]);
    if (!defAccessor) {
        return -1;
    }
    auto** defVt = *reinterpret_cast<void***>(defAccessor);
    if (!defVt || !defVt[3]) {
        return -1;
    }
    void* argRange = reinterpret_cast<GetArgsFn>(defVt[3])(defAccessor);
    if (!argRange) {
        return -1;
    }

    alignas(16) uint8_t proc[64]{};
    *reinterpret_cast<void**>(proc) = g_argProcessorVftable;
    *reinterpret_cast<void**>(proc + 8) = argRange;
    sharedAssign(proc + 16, params + 3);
    SharedPointerPairGuard procSharedGuard{proc + 16};

    constexpr char kArgName[] = "RouteType";
    const char* keyDesc[2]{kArgName, kArgName + sizeof(kArgName) - 1};
    alignas(16) uint8_t nameOut[48]{};
    const int64_t strPtr = getStringValue(proc, nameOut, keyDesc, nullptr);
    char nameBuf[128]{};
    if (strPtr) {
        const char* nameStr = reinterpret_cast<const char*>(strPtr);
        if (*reinterpret_cast<uint64_t*>(nameOut + 24) >= 0x10) {
            nameStr = *reinterpret_cast<const char**>(nameOut);
        }
        if (nameStr) {
            strncpy_s(nameBuf, sizeof(nameBuf), nameStr, _TRUNCATE);
        }
    }
    // Copy the name while the processor's shared definition is still alive;
    // the guard then releases that temporary reference on every return path.
    if (!nameBuf[0]) {
        return -1;
    }
    if (g_api) {
        g_api->Log("PlotRouteTypeMatches: route RouteType=%s", nameBuf);
    }
    return RouteTypeIndexFromName(nameBuf);
}


int __fastcall Hook_RouteTestPlot(void* req, void* plot) {
    if (!req || !plot) {
        return 3;
    }
    const int actual = static_cast<int>(*reinterpret_cast<int8_t*>(
        static_cast<char*>(plot) + kRouteTypePlotOffset));
    // Priority: tracked Create-time index → pending Create index → +0xA8.
    // Native Terrain OnInitialize writes -1 into +0xA8; Test runs during Create
    // before TrackWeakRequirement, so g_pendingRouteIndex covers that window.
    int expected = FindTrackedRouteIndex(req);
    if (expected < 0 && g_pendingRouteIndex >= 0) {
        expected = g_pendingRouteIndex;
    }
    if (expected < 0) {
        expected = *reinterpret_cast<int*>(
            static_cast<char*>(req) + kRouteTypeRequirementOffset);
    }
    const int state = (expected >= 0 && actual == expected) ? 2 : 3;
    static volatile LONG s_routeTestLogBudget = 32;
    if (g_api && s_routeTestLogBudget > 0
        && _InterlockedDecrement(&s_routeTestLogBudget) >= 0) {
        g_api->Log(
            "PlotRouteTypeMatches: route TestPlot req=%p plot=%p actual=%d expected=%d pending=%d state=%d",
            req,
            plot,
            actual,
            expected,
            g_pendingRouteIndex,
            state);
    }
    return state;
}


bool IsTrackedRouteRequirement(void* req) {
    if (!req) {
        return false;
    }
    AcquireSRWLockShared(&g_weakRequirementLock);
    for (size_t i = 0; i < g_weakRequirementCount; ++i) {
        if (g_weakRequirements[i].object == req) {
            ReleaseSRWLockShared(&g_weakRequirementLock);
            return true;
        }
    }
    ReleaseSRWLockShared(&g_weakRequirementLock);
    return false;
}


int __fastcall Detour_RouteTestPlot(void* req, void* plot) {
    // Prefer the weak-tracked set over definition-hash QI: avoids touching
    // arbitrary Terrain instances' definition pointers on every TestPlot.
    if (IsTrackedRouteRequirement(req) || IsRouteRequirement(req)) {
        return Hook_RouteTestPlot(req, plot);
    }
    return g_origRouteTestPlot ? g_origRouteTestPlot(req, plot) : 3;
}


int __fastcall Detour_PlotRequirementTest(void* req, void* a2, void* a3) {
    // Force 0/1 Met encoding for our route instances.  Raw TestPlot 2/3 left in
    // GetState() is ignored by consumers that only treat 0 as Met.
    if (IsTrackedRouteRequirement(req) || IsRouteRequirement(req)) {
        const int raw = g_origPlotRequirementTest
            ? g_origPlotRequirementTest(req, a2, a3)
            : 3;
        return NormalizeRequirementStateFromTestPlot(raw);
    }
    return g_origPlotRequirementTest ? g_origPlotRequirementTest(req, a2, a3) : 1;
}


const char* __fastcall Hook_RouteGetTypeName(void* /*self*/) {
    return "REQUIREMENT_PLOT_ROUTE_TYPE_MATCHES";
}


int __fastcall Hook_RouteValidateDefinition(void* /*self*/, const void* definitionSharedPtr) {
    void* definition = definitionSharedPtr
        ? *reinterpret_cast<void* const*>(definitionSharedPtr)
        : nullptr;
    return GetDefinitionTypeHash(definition) == g_routeRequirementTypeHash ? 0 : 4;
}

struct PendingRouteIndexScope {
    int previous;

    explicit PendingRouteIndexScope(int index)
        : previous(g_pendingRouteIndex) {
        g_pendingRouteIndex = index;
    }

    ~PendingRouteIndexScope() {
        g_pendingRouteIndex = previous;
    }
};

void* __fastcall Hook_RouteCreate(void* self, void* outSharedPtr, const void* params) {
    if (!g_createRouteFactory) {
        *reinterpret_cast<void**>(outSharedPtr) = nullptr;
        *(reinterpret_cast<void**>(outSharedPtr) + 1) = nullptr;
        return outSharedPtr;
    }
    // Resolve RouteType BEFORE native Create: Initialize() calls TestPlot while
    // +0xA8 is still -1 and before TrackWeakRequirement runs.
    const int resolvedIndex = params
        ? ResolveRouteTypeIndex(reinterpret_cast<int64_t*>(const_cast<void*>(params)))
        : -1;
    if (g_api) {
        g_api->Log(
            "PlotRouteTypeMatches: route Create begin factory=%p out=%p params=%p pendingIndex=%d",
            self,
            outSharedPtr,
            params,
            resolvedIndex);
    }
    PendingRouteIndexScope pendingScope(resolvedIndex);
    g_createRouteFactory(self, outSharedPtr, params);

    void* requirement = *reinterpret_cast<void**>(outSharedPtr);
    bool parsed = resolvedIndex >= 0;
    bool tracked = false;
    if (requirement && parsed) {
        *reinterpret_cast<int*>(
            static_cast<char*>(requirement) + kRouteTypeRequirementOffset) = resolvedIndex;
        tracked = TrackWeakRequirement(
            requirement, resolvedIndex);
        // Re-evaluate: Create-time Test may have run before pending was visible
        // to this instance's first path, or state needs sync after +0xA8 write.
        if (auto** vt = *reinterpret_cast<void***>(requirement)) {
            if (vt[22] && g_setState) {
                using TestFn = int(__fastcall*)(void*, void*, void*);
                const int state =
                    reinterpret_cast<TestFn>(vt[22])(requirement, nullptr, nullptr);
                g_setState(requirement, state, 1);
                if (g_api) {
                    g_api->Log(
                        "PlotRouteTypeMatches: route Create refresh SetState req=%p state=%d",
                        requirement,
                        state);
                }
            }
        }
    } else if (requirement && g_api) {
        g_api->Log(
            "PlotRouteTypeMatches: route Create failed to parse RouteType req=%p",
            requirement);
    }
    if (g_api) {
        g_api->Log(
            "PlotRouteTypeMatches: route Create done req=%p parsed=%u tracked=%u index=%d",
            requirement,
            static_cast<unsigned>(parsed),
            static_cast<unsigned>(tracked),
            resolvedIndex);
    }
    return outSharedPtr;
}

bool BuildRouteFactoryVtable(HookAPI* api) {
    RequirementFactoryObject* donor = nullptr;
    if (!FindFactoryObjectByTypeName(
            "REQUIREMENT_PLOT_TERRAIN_TYPE_MATCHES", &donor)
        || !donor || !donor->vftable) {
        if (api) {
            api->Log("PlotRouteTypeMatches: native PlotTerrain factory not found");
        }
        return false;
    }

    auto** src = donor->vftable;
    g_createRouteFactory = reinterpret_cast<RouteFactoryCreateFn>(src[kFactoryCreateSlot]);
    g_ourRouteFactoryVftable[-1] = src[-1];
    std::memcpy(g_ourRouteFactoryVftable, src, 6 * sizeof(void*));
    g_ourRouteFactoryVftable[kFactoryGetTypeNameSlot] = reinterpret_cast<void*>(&Hook_RouteGetTypeName);
    g_ourRouteFactoryVftable[kFactoryCreateSlot] = reinterpret_cast<void*>(&Hook_RouteCreate);
    g_ourRouteFactoryVftable[kFactoryValidateDefinitionSlot] = reinterpret_cast<void*>(&Hook_RouteValidateDefinition);
    g_routeVtableReady = g_createRouteFactory != nullptr;
    if (api) {
        api->Log(
            "PlotRouteTypeMatches: PlotTerrain donor=%p create=%p routeFactoryVF=%p",
            donor,
            g_createRouteFactory,
            g_ourRouteFactoryVftable);
    }
    return g_routeVtableReady;
}

bool FindFactoryObjectByTypeName(const char* wantedName, RequirementFactoryObject** outFactory) {
    if (outFactory) {
        *outFactory = nullptr;
    }
    if (!g_getTypes) {
        return false;
    }
    auto* types = reinterpret_cast<StdVectorPtr*>(g_getTypes());
    if (!types || !types->begin || !types->end) {
        return false;
    }
    for (void** it = types->begin; it != types->end; ++it) {
        auto* factory = static_cast<RequirementFactoryObject*>(*it);
        if (!factory || !factory->vftable) {
            continue;
        }
        using GetNameFn = const char*(__fastcall*)(void*);
        auto* getName = reinterpret_cast<GetNameFn>(factory->vftable[kFactoryGetTypeNameSlot]);
        if (!getName) {
            continue;
        }
        const char* name = getName(factory);
        if (name && wantedName && std::strcmp(name, wantedName) == 0) {
            if (outFactory) {
                *outFactory = factory;
            }
            return true;
        }
    }
    return false;
}

bool FindFactoryByTypeName(const char* wantedName) {
    return FindFactoryObjectByTypeName(wantedName, nullptr);
}

bool PushRouteFactory(HookAPI* api) {
    if (!g_getTypes || !g_routeVtableReady) {
        return false;
    }
    auto* types = reinterpret_cast<StdVectorPtr*>(g_getTypes());
    if (!types) {
        return false;
    }
    constexpr const char* kRouteRequirementTypeName =
        "REQUIREMENT_PLOT_ROUTE_TYPE_MATCHES";
    if (FindFactoryByTypeName(kRouteRequirementTypeName)) {
        api->Log("PlotRouteTypeMatches: route factory already present, skip push");
        return true;
    }

    if (types->end == types->capacity) {
        api->Log("PlotRouteTypeMatches: Registry::GetTypes() full");
        return false;
    }
    // The native registry owns deletion, so use the native allocator.
    if (!g_opNew) return false;
    void* mem = g_opNew(sizeof(RequirementFactoryObject));
    if (!mem) return false;
    std::memset(mem, 0, 8);
    auto* factory = static_cast<RequirementFactoryObject*>(mem);
    factory->vftable = g_ourRouteFactoryVftable;
    *types->end = factory;
    ++types->end;
    api->Log(
        "PlotRouteTypeMatches: registered %s factory=%p",
        kRouteRequirementTypeName,
        factory);
    return true;
}


}  // namespace

bool NewRequirements_RegisterPlotRoute(HookAPI* api) {
    if (!HookAPI_IsCompatible(api)) {
        return false;
    }
    g_api = api;
    g_getTypes = reinterpret_cast<GetTypesFn>(ResolveSpec(api, kSymGetTypes));
    g_routeTestPlot = ResolveSpec(api, kSymRouteTestPlot);
    g_plotRequirementTest = ResolveSpec(api, kSymPlotRequirementTest);
    g_setRouteTypeOnPlot = ResolveSpec(api, kSymSetRouteTypeOnPlot);
    g_routeGetDefinitions = ResolveSpec(api, kSymRouteGetDefinitions);
    g_opNew = reinterpret_cast<OpNewFn>(ResolveSpec(api, kSymOpNew));
    g_makeHash = reinterpret_cast<MakeHashFn>(ResolveSpec(api, kSymMakeHash));
    g_setState = reinterpret_cast<SetStateFn>(ResolveSpec(api, kSymSetState));
    g_argProcessorVftable = ResolveSpec(api, kSymArgProcessorVftable);

    if (!LoadGuidFromImage(api, kSymIPlotInterfaceGuid, g_iPlotInterfaceGuid)) {
        api->Log("PlotRouteTypeMatches: failed to load subject interface GUID");
        return false;
    }
    if (g_makeHash) {
        g_routeRequirementTypeHash = g_makeHash("REQUIREMENT_PLOT_ROUTE_TYPE_MATCHES");
    }

    if (!g_getTypes || !g_routeTestPlot || !g_plotRequirementTest
        || !g_setRouteTypeOnPlot || !g_routeGetDefinitions || !g_makeHash
        || !g_opNew || !g_setState || !g_argProcessorVftable || !g_routeRequirementTypeHash) {
        api->Log("PlotRouteTypeMatches: missing critical symbols");
        return false;
    }
    if (!BuildRouteFactoryVtable(api)) {
        api->Log("PlotRouteTypeMatches: failed to build factory vtable");
        return false;
    }

    if (!api->InstallHook(
            g_routeTestPlot,
            reinterpret_cast<void*>(&Detour_RouteTestPlot),
            reinterpret_cast<void**>(&g_origRouteTestPlot))) {
        api->Log("PlotRouteTypeMatches: failed to install Detour_RouteTestPlot");
        return false;
    }
    api->Log("PlotRouteTypeMatches: Detour_RouteTestPlot installed at %p", g_routeTestPlot);
    if (!api->InstallHook(
            g_plotRequirementTest,
            reinterpret_cast<void*>(&Detour_PlotRequirementTest),
            reinterpret_cast<void**>(&g_origPlotRequirementTest))) {
        api->Log("PlotRouteTypeMatches: failed to install Detour_PlotRequirementTest");
        return false;
    }
    api->Log("PlotRouteTypeMatches: Detour_PlotRequirementTest installed at %p", g_plotRequirementTest);
    if (!api->InstallHook(
            g_setRouteTypeOnPlot,
            reinterpret_cast<void*>(&Hook_SetRouteTypeOnPlot),
            reinterpret_cast<void**>(&g_origSetRouteTypeOnPlot))) {
        api->Log("PlotRouteTypeMatches: failed to install Hook_SetRouteTypeOnPlot");
        return false;
    }
    api->Log("PlotRouteTypeMatches: Hook_SetRouteTypeOnPlot installed at %p", g_setRouteTypeOnPlot);
    const bool registered = PushRouteFactory(api);
    api->Log("PlotRouteTypeMatches: registration %s", registered ? "OK" : "FAILED");
    return registered;
}

void NewRequirements_UnloadPlotRoute() {
    ClearWeakRequirements();
}
