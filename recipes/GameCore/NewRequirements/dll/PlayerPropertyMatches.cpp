#include "pch.h"
#include "NewRequirements.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <new>

// REQUIREMENT_PLAYER_PROPERTY_MATCHES
// Self-contained teaching example: addresses, state, factory, parsing and hooks
// all live here. Helpers are intentionally duplicated in the other REQ file.
// Only the registration function below has external linkage.
// Keep the native PlotProperty instance vtable and lifetime; filter every detour.
namespace {

constexpr const char* kRequirementTypeName = "REQUIREMENT_PLAYER_PROPERTY_MATCHES";
constexpr int kPlayerPropsOffset = 0x518;
constexpr int kOffActive = 0x1C;
constexpr int kOffModifierContext = 0x40;
constexpr int kOffKeyHash = 0xB8;
constexpr int kOffMinimum = 0xBC;
constexpr int kFactoryGetTypeNameSlot = 1;
constexpr int kFactoryCreateSlot = 3;
constexpr int kFactoryValidateDefinitionSlot = 5;
constexpr int kGameEditSignalHubOffset = 0x280;
constexpr int kPlayerPropSignalOffset = 0x960;

struct SymbolSpec {
    const char* name;        // demangled symbol name; empty for data-only symbols
    uintptr_t currentRva;    // current Steam GameCore coordinate, used only as fallback
};

const SymbolSpec kSymGetTypes{"GameEffects::IRequirementFactory>::GetTypes", 0x626740};
const SymbolSpec kSymCreatePlotFactory{"GameEffects::Requirements::PlotPropertyMatches>::Create", 0x8813F0};
const SymbolSpec kSymGetTypeNamePlotFactory{"GameEffects::Requirements::PlotPropertyMatches>::GetTypeName", 0x880090};
const SymbolSpec kSymValidatePlotFactory{"GameEffects::Requirements::PlotPropertyMatches>::ValidateDefinition", 0x882AB0};
const SymbolSpec kSymInitPlot{"GameEffects::Requirements::PlotPropertyMatches::OnInitialize", 0x8F4A30};
const SymbolSpec kSymTestPlot{"GameEffects::Requirements::PlotPropertyMatches::Test", 0x8F5060};
const SymbolSpec kSymSetState{"GameEffects::Requirements::SimpleRequirement::SetState", 0x9755E0};
const SymbolSpec kSymPlotFactoryVftable{"", 0xA8BBA0};
const SymbolSpec kSymPlotRequirementVftable{"", 0xA6BDD0};
const SymbolSpec kSymIPlayerInterfaceGuid{"", 0xA9A4F8};
const SymbolSpec kSymMakeHash{"GameCore::Utilities::MakeHash", 0x606270};
const SymbolSpec kSymFAutoEdit{"GameCore::Game::Instance>::edit", 0x72A920};
const SymbolSpec kSymGameEdit{"GameCore::Game::Instance::Edit", 0x951550};
const SymbolSpec kSymOpNew{"operator_new", 0x990900};
const SymbolSpec kSymArgGetStringValue{"GameEffects::ArgumentProcessor::GetStringValue", 0x9565C0};
const SymbolSpec kSymArgParse{"GameEffects::ArgumentProcessor::Parse", 0x96F5C0};
const SymbolSpec kSymArgGetFunctions{"GameEffects::ArgumentFunction::GetFunctions", 0x9514E0};
const SymbolSpec kSymArgFindIf{"_lambda_3b9a652386d0677efa8a1746cf4950ca_>", 0x9209E0};
const SymbolSpec kSymSharedPtrAssign{"GameEffects::IModifierDefinition>::operator=", 0x6D8DE0};
const SymbolSpec kSymArgProcessorVftable{"", 0xAA39E0};
const SymbolSpec kSymPlayerPropDispatch{"GameCore::Message::SignalBase::ProtoSignal_void::_cdecl(class_GameCore::Player::Instance::::ptr64_enum_GameCore::AgendaTypes_enum_GameCore::DiplomaticVisibilityTypes)_struct_GameCore::Message::SignalBase::CollectorDefault_void::_::Dispatch", 0x375C80};
const SymbolSpec kSymLuaPlayerSetProperty{"GameCore::Lua::IPlayer::lSetProperty", 0x6EE3F0};
const SymbolSpec kSymLuaGetPlayerInstance{"GameCore::Lua::IPlayer::GetPlayerInstance", 0x6E8120};
const SymbolSpec kSymLuaIsNumber{"hksi_lua_isnumber", 0x154B0};
const SymbolSpec kSymLuaIsString{"hksi_lua_isstring", 0x15570};
const SymbolSpec kSymLuaToInteger{"hksi_lua_tointeger", 0xDC20};
const SymbolSpec kSymLuaToString{"hksi_lua_tolstring", 0x990250};

HookAPI* g_api = nullptr;

using GetTypesFn = void*(__cdecl*)();
using FactoryCreateFn = void*(__fastcall*)(void* self, void* outSharedPtr, const void* params);
using FAutoEditFn = void*(__fastcall*)(void* autoVar);
using OpNewFn = void*(__cdecl*)(size_t);
using GameEditFn = void*(__cdecl*)();
using MakeHashFn = uint32_t(__cdecl*)(const char*);
using SetStateFn = void(__fastcall*)(void* req, int state, char notify);
using TestFn = int(__fastcall*)(void* req, void* a2, void* a3);
using OnInitializeFn = char(__fastcall*)(void* req, int64_t* params, int64_t ctx);
using PlayerPropDispatchFn = void(__fastcall*)(void*, void*, unsigned int, int);
using LuaPlayerSetPropertyFn = int(__fastcall*)(void*);
using LuaGetPlayerInstanceFn = void*(__fastcall*)(void*);
using LuaPredicateFn = int(__cdecl*)(void*, int);
using LuaToIntegerFn = int(__cdecl*)(void*, int);
using LuaToStringFn = const char*(__cdecl*)(void*, int, size_t*);
GetTypesFn g_getTypes = nullptr;
FactoryCreateFn g_createPlotFactory = nullptr;
FAutoEditFn g_fautoEdit = nullptr;
OpNewFn g_opNew = nullptr;
GameEditFn g_gameEdit = nullptr;
MakeHashFn g_makeHash = nullptr;
SetStateFn g_setState = nullptr;
void* g_plotFactoryVftable = nullptr;
void* g_getTypeNamePlot = nullptr;
void* g_validateDefinitionPlot = nullptr;
void* g_onInitializePlot = nullptr;
void* g_testPlot = nullptr;
void* g_argProcessorVftable = nullptr;
void* g_plotPropertyRequirementVftable = nullptr;
OnInitializeFn g_origPlotOnInitialize = nullptr;
TestFn g_origPlotTest = nullptr;
PlayerPropDispatchFn g_origPlayerPropDispatch = nullptr;
LuaPlayerSetPropertyFn g_origLuaPlayerSetProperty = nullptr;
LuaGetPlayerInstanceFn g_luaGetPlayerInstance = nullptr;
LuaPredicateFn g_luaIsNumber = nullptr;
LuaPredicateFn g_luaIsString = nullptr;
LuaToIntegerFn g_luaToInteger = nullptr;
LuaToStringFn g_luaToString = nullptr;
uint32_t g_requirementTypeHash = 0;

// Preserve the donor factory's RTTI entry at vtable[-1].
thread_local bool g_customCreateActive = false;
alignas(16) uint8_t g_iPlayerInterfaceGuid[16]{};
void* g_ourFactoryVftableStorage[7]{};
void** const g_ourFactoryVftable = g_ourFactoryVftableStorage + 1;
bool g_vtablesReady = false;
int g_requirementTestSlot = -1;
int __fastcall Hook_Test(void* req, void* a2, void* a3);

struct WeakRequirementRef {
    void* object;
    void* control;
};

constexpr size_t kMaxWeakRequirements = 4096;
WeakRequirementRef g_weakRequirements[kMaxWeakRequirements]{};
size_t g_weakRequirementCount = 0;
SRWLOCK g_weakRequirementLock = SRWLOCK_INIT;

struct RequirementFactoryObject { void** vftable; };
struct StdVectorPtr {
    void** begin;
    void** end;
    void** capacity;
};
bool FindFactoryObjectByTypeName(const char* wantedName, RequirementFactoryObject** outFactory);

// This project's HookAPI.h intentionally retains the legacy ABI layout.
// Probe the appended current-image resolver only when structSize permits it.
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

bool TrackWeakRequirement(void* req) {
    if (!req) {
        return false;
    }
    // MSVC enable_shared_from_this weak_this occupies req+8: object/control.
    WeakRequirementRef weak{
        *reinterpret_cast<void**>(static_cast<char*>(req) + 8),
        *reinterpret_cast<void**>(static_cast<char*>(req) + 16)};
    if (!weak.object || !weak.control) {
        if (g_api) {
            g_api->Log(
                "PlayerPropertyMatches: weak_this unavailable req=%p object=%p control=%p",
                req, weak.object, weak.control);
        }
        return false;
    }

    AcquireSRWLockExclusive(&g_weakRequirementLock);
    for (size_t i = 0; i < g_weakRequirementCount; ++i) {
        if (g_weakRequirements[i].object == weak.object
            && g_weakRequirements[i].control == weak.control) {

            ReleaseSRWLockExclusive(&g_weakRequirementLock);
            return true;
        }
    }
    if (g_weakRequirementCount >= kMaxWeakRequirements) {
        ReleaseSRWLockExclusive(&g_weakRequirementLock);
        if (g_api) {
            g_api->Log("PlayerPropertyMatches: weak registry full");
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
            "PlayerPropertyMatches: weak tracked req=%p control=%p count=%llu",
            weak.object,
            weak.control,
            static_cast<unsigned long long>(count));
    }
    return true;
}

int FindSlot(void** vftable, void* fn, int maxSlots) {
    if (!vftable || !fn) {
        return -1;
    }
    for (int i = 0; i < maxSlots; ++i) {
        if (vftable[i] == fn) {
            return i;
        }
    }
    return -1;
}


void* GetPlayerFromIPlayerInterface(void* qiResult) {
    if (!qiResult || qiResult == reinterpret_cast<void*>(8)) {
        return nullptr;
    }
    void* iface = static_cast<char*>(qiResult) - 8;
    auto** vt = *reinterpret_cast<void***>(iface);
    using GetPlayerFn = void*(__fastcall*)(void*);
    // QueryInterface returns the interface payload (+8).  The player-interface
    // reference implementation (PlayerMetXTurnsAgo) obtains the player through
    // vtable + 8, i.e. virtual slot 1.
    return reinterpret_cast<GetPlayerFn>(vt[1])(iface);
}


void* GetTrackedPlayer(void* req) {
    void* ctx = *reinterpret_cast<void**>(static_cast<char*>(req) + kOffModifierContext);
    if (!ctx) {
        return nullptr;
    }
    void* qi = QueryInterface(ctx, g_iPlayerInterfaceGuid, true);
    if (!qi || qi == reinterpret_cast<void*>(8)) {
        return nullptr;
    }
    return GetPlayerFromIPlayerInterface(qi);
}


void* EditPlayerProperties(void* player) {
    if (!player || !g_fautoEdit) {
        return nullptr;
    }
    return g_fautoEdit(static_cast<char*>(player) + kPlayerPropsOffset);
}


bool IsCustomRequirement(void* req) {
    if (!req || !g_requirementTypeHash) {
        return false;
    }
    void* definition = *reinterpret_cast<void**>(static_cast<char*>(req) + 0x20);
    return GetDefinitionTypeHash(definition) == g_requirementTypeHash;
}


void RefreshOneRequirement(
    void* req, void* player, unsigned int keyHash, int /*delta*/) {
    if (g_api) {
        g_api->Log(
            "PlayerPropertyMatches: PlayerPropertyChanged req=%p player=%p key=%08X",
            req, player, keyHash);
    }
    if (!req || !player) {
        return;
    }
    const uint32_t watched = *reinterpret_cast<uint32_t*>(static_cast<char*>(req) + kOffKeyHash);
    if (keyHash != watched) {
        return;
    }
    void* tracked = GetTrackedPlayer(req);
    if (!tracked || tracked != player) {
        return;
    }
    if (!*reinterpret_cast<uint8_t*>(static_cast<char*>(req) + kOffActive)) {
        return;
    }
    InvokeSetState(req, Hook_Test(req, nullptr, nullptr));
}


void RefreshTrackedRequirements(void* player, uint32_t keyHash, int delta) {
    WeakRequirementRef snapshot[kMaxWeakRequirements]{};
    AcquireSRWLockShared(&g_weakRequirementLock);
    const size_t count = g_weakRequirementCount;
    std::memcpy(snapshot, g_weakRequirements, count * sizeof(WeakRequirementRef));
    ReleaseSRWLockShared(&g_weakRequirementLock);

    if (g_api) {
        g_api->Log(
            "PlayerPropertyMatches: refresh player=%p key=%08X tracked=%llu",
            player, keyHash, static_cast<unsigned long long>(count));
    }
    for (size_t i = 0; i < count; ++i) {
        void* req = TryLockWeakReference(snapshot[i]);
        if (!req) {
            continue;
        }
        RefreshOneRequirement(req, player, keyHash, delta);
        ReleaseStrongReference(snapshot[i].control);
    }
}


void* GetPlayerPropertySignal() {
    if (!g_gameEdit) {
        return nullptr;
    }
    void* game = g_gameEdit();
    void* hub = game
        ? *reinterpret_cast<void**>(static_cast<char*>(game) + kGameEditSignalHubOffset)
        : nullptr;
    return hub ? static_cast<char*>(hub) + kPlayerPropSignalOffset : nullptr;
}


void __fastcall Hook_PlayerPropDispatch(
    void* signal, void* player, unsigned int keyHash, int delta) {
    g_origPlayerPropDispatch(signal, player, keyHash, delta);
    if (signal == GetPlayerPropertySignal()) {
        RefreshTrackedRequirements(player, keyHash, delta);
    }
}


bool GetLuaPropertyKey(void* luaState, uint32_t* outKey) {
    if (!luaState || !outKey || !g_luaIsNumber || !g_luaIsString
        || !g_luaToInteger || !g_luaToString || !g_makeHash) {
        return false;
    }
    // Utility::SetProperty checks the second Lua argument as number first,
    // then as string. Mirror that order so the emitted key matches the write.
    if (g_luaIsNumber(luaState, 2)) {
        *outKey = static_cast<uint32_t>(g_luaToInteger(luaState, 2));
        return true;
    }
    if (g_luaIsString(luaState, 2)) {
        size_t length = 0;
        const char* key = g_luaToString(luaState, 2, &length);
        if (key) {
            *outKey = g_makeHash(key);
            return true;
        }
    }
    return false;
}


int __fastcall Hook_LuaPlayerSetProperty(void* luaState) {
    void* player = g_luaGetPlayerInstance ? g_luaGetPlayerInstance(luaState) : nullptr;
    uint32_t keyHash = 0;
    const bool hasKey = GetLuaPropertyKey(luaState, &keyHash);
    const int result = g_origLuaPlayerSetProperty(luaState);
    if (g_api) {
        g_api->Log(
            "PlayerPropertyMatches: Lua Player:SetProperty player=%p key=%08X valid=%u",
            player, keyHash, static_cast<unsigned>(hasKey));
    }
    if (player && hasKey) {
        RefreshTrackedRequirements(player, keyHash, 0);
    }
    return result;
}


bool ParsePropertyNameMinimum(void* req, int64_t* params) {
    if (!g_makeHash || !g_argProcessorVftable) {
        return false;
    }

    using GetArgsFn = void*(__fastcall*)(void* defAccessor);
    using GetStringValueFn = int64_t(__fastcall*)(void* proc, void* outStr, void* keyDesc, void* fallback);
    using ParseFn = char(__fastcall*)(void* argEntry, int* outValue);
    using GetFunctionsFn = void*(__cdecl*)();
    using FindIfFn = void*(__fastcall*)(void* begin, void* end, void* predicateValue);
    using AssignFn = void(__fastcall*)(void* dst, void* src);

    auto* getStringValue = reinterpret_cast<GetStringValueFn>(ResolveSpec(g_api, kSymArgGetStringValue));
    auto* parseArg = reinterpret_cast<ParseFn>(ResolveSpec(g_api, kSymArgParse));
    auto* getFunctions = reinterpret_cast<GetFunctionsFn>(ResolveSpec(g_api, kSymArgGetFunctions));
    auto* findIf = reinterpret_cast<FindIfFn>(ResolveSpec(g_api, kSymArgFindIf));
    auto* sharedAssign = reinterpret_cast<AssignFn>(ResolveSpec(g_api, kSymSharedPtrAssign));

    if (!getStringValue || !parseArg || !getFunctions || !findIf) {
        if (g_api) {
            g_api->Log("PlayerPropertyMatches: ArgumentProcessor helpers unresolved");
        }
        return false;
    }

    void* defAccessor = reinterpret_cast<void*>(params[1]);
    if (!defAccessor) {
        return false;
    }

    auto** defVt = *reinterpret_cast<void***>(defAccessor);
    void* argRange = reinterpret_cast<GetArgsFn>(defVt[3])(defAccessor);
    if (!argRange) {
        return false;
    }

    if (g_api) {
        g_api->Log("PlayerPropertyMatches: Parse: argument range=%p", argRange);
    }

    // The decompiled ArgumentProcessor stack object is laid out as:
    //   +0  vtable
    //   +8  RequirementDefinition::GetArguments() result
    //   +16 shared_ptr copied from RequirementParameters + 0x18
    // GetStringValue reads these members from the processor itself.
    alignas(16) uint8_t proc[64]{};
    *reinterpret_cast<void**>(proc) = g_argProcessorVftable;
    *reinterpret_cast<void**>(proc + 8) = argRange;
    if (sharedAssign) {
        sharedAssign(proc + 16, params + 3);
    }
    SharedPointerPairGuard procSharedGuard{proc + 16};

    // GetStringValue expects a string-view pair {begin, end}.  IDA displays
    // vanilla's end pointer as "" because the linker placed that empty string
    // at the terminator of "PropertyName"; two unrelated literals are not a
    // valid replacement for that layout.
    constexpr char keyName[] = "PropertyName";
    const char* keyNameDesc[2]{keyName, keyName + sizeof(keyName) - 1};
    alignas(16) uint8_t nameOut[48]{};
    int64_t strPtr = getStringValue(proc, nameOut, keyNameDesc, nullptr);
    if (!strPtr) {
        if (g_api) {
            g_api->Log("PlayerPropertyMatches: Parse: PropertyName lookup returned null");
        }
        return false;
    }
    const char* nameStr = reinterpret_cast<const char*>(strPtr);
    if (*reinterpret_cast<uint64_t*>(nameOut + 24) >= 0x10) {
        nameStr = *reinterpret_cast<const char**>(nameOut);
    }
    if (!nameStr) {
        return false;
    }
    *reinterpret_cast<uint32_t*>(static_cast<char*>(req) + kOffKeyHash) = g_makeHash(nameStr);
    if (g_api) {
        g_api->Log("PlayerPropertyMatches: Parse: PropertyName=%s", nameStr);
    }

    void** begin = *reinterpret_cast<void***>(argRange);
    void** end = reinterpret_cast<void***>(argRange)[1];
    if (g_api) {
        g_api->Log("PlayerPropertyMatches: Parse: entries begin=%p end=%p", begin, end);
    }
    int minimum = 0;
    for (void** it = begin; it != end; it += 20) {
        const char* argName = (reinterpret_cast<uintptr_t>(it[3]) < 0x10)
                                  ? reinterpret_cast<const char*>(it)
                                  : reinterpret_cast<const char*>(*reinterpret_cast<void**>(it));
        if (g_api) {
            g_api->Log("PlayerPropertyMatches: Parse: examining argument=%s entry=%p", argName, it);
        }
        if (std::strcmp(argName, "PropertyMinimum") != 0) {
            continue;
        }
        int parsed = 0;
        int parsedAlt = 0;
        if (g_api) {
            g_api->Log("PlayerPropertyMatches: Parse: parsing PropertyMinimum entry=%p", it);
        }
        if (!parseArg(reinterpret_cast<void*>(it + 4), &parsed)) {
            break;
        }
        if (g_api) {
            g_api->Log("PlayerPropertyMatches: Parse: PropertyMinimum parsed raw=%d evaluator=%p", parsed, it[10]);
        }
        if (it[10]) {
            void** functions = static_cast<void**>(getFunctions());
            void** key = it + 8;
            // Native code stores (entry + 0x40) in a local pointer and passes
            // the address of that local to std::find_if.  Its predicate thus
            // expects void*** here, not the string object address directly.
            void* found = findIf(functions[0], functions[1], &key);
            if (found && found != functions[1]) {
                using EvalFn = char(__fastcall*)(void*, int*, void*, void*, int*, void*);
                // RequirementDefinitionFunctions invokes its evaluator at
                // vtable + 24 (slot 3). find_if returns an iterator whose
                // element is the evaluator pointer, so two dereferences are
                // required before indexing that vtable.
                void* evaluator = *reinterpret_cast<void**>(found);
                auto** evaluatorVt = evaluator
                    ? *reinterpret_cast<void***>(evaluator)
                    : nullptr;
                auto* eval = evaluatorVt
                    ? reinterpret_cast<EvalFn>(evaluatorVt[3])
                    : nullptr;
                if (eval
                    && eval(
                        evaluator,
                        &parsed,
                        it + 12,
                        it + 16,
                        &parsedAlt,
                        proc + 16)) {
                    minimum = parsedAlt;
                } else {
                    minimum = parsed;
                }
            } else {
                minimum = parsed;
            }
        } else {
            minimum = parsed;
        }
        break;
    }

    *reinterpret_cast<int*>(static_cast<char*>(req) + kOffMinimum) = minimum >> 8;
    if (g_api) {
        g_api->Log("PlayerPropertyMatches: Parse: PropertyMinimum=%d", minimum >> 8);
    }
    return true;
}


char __fastcall Hook_OnInitialize(void* req, int64_t* params, int64_t /*ctx*/) {
    if (!req || !params) {
        return 0;
    }
    if (g_api) {
        g_api->Log("PlayerPropertyMatches: OnInitialize begin req=%p params=%p", req, params);
    }

    void* ctx = *reinterpret_cast<void**>(static_cast<char*>(req) + kOffModifierContext);
    if (!ctx) {
        return 0;
    }
    void* qi = QueryInterface(ctx, g_iPlayerInterfaceGuid, true);
    if (!qi || qi == reinterpret_cast<void*>(8)) {
        if (g_api) {
            g_api->Log("PlayerPropertyMatches: OnInitialize: IPlayerInterface unavailable req=%p qi=%p", req, qi);
        }
        return 0;
    }

    if (!ParsePropertyNameMinimum(req, params)) {
        if (g_api) {
            g_api->Log("PlayerPropertyMatches: ParsePropertyNameMinimum failed");
        }
        return 0;
    }
    if (g_api) {
        g_api->Log("PlayerPropertyMatches: OnInitialize: arguments parsed req=%p", req);
    }

    if (!TrackWeakRequirement(req)) {
        if (g_api) {
            g_api->Log("PlayerPropertyMatches: weak tracking failed");
        }
        return 0;
    }

    if (g_api) {
        g_api->Log("PlayerPropertyMatches: OnInitialize complete req=%p", req);
    }

    return 1;
}


int __fastcall Hook_Test(void* self, void* /*a2*/, void* /*a3*/) {
    if (!self) {
        return 3;
    }
    if (g_api) {
        g_api->Log("PlayerPropertyMatches: Test begin req=%p", self);
    }

    void* player = GetTrackedPlayer(self);
    if (!player) {
        if (g_api) {
            g_api->Log("PlayerPropertyMatches: Test: tracked player unavailable req=%p", self);
        }
        return 3;
    }
    if (g_api) {
        g_api->Log("PlayerPropertyMatches: Test: tracked player=%p req=%p", player, self);
    }

    void* props = EditPlayerProperties(player);
    if (!props) {
        if (g_api) {
            g_api->Log("PlayerPropertyMatches: Test: property container unavailable req=%p player=%p", self, player);
        }
        return 1;
    }
    if (g_api) {
        g_api->Log("PlayerPropertyMatches: Test: property container=%p", props);
    }

    const uint32_t keyHash = *reinterpret_cast<uint32_t*>(static_cast<char*>(self) + kOffKeyHash);
    const int minimum = *reinterpret_cast<int*>(static_cast<char*>(self) + kOffMinimum);

    uint16_t* propCell = nullptr;
    if (auto** vt = *reinterpret_cast<void***>(props)) {
        using GetPropFn = uint16_t*(__fastcall*)(void*, uint32_t);
        if (vt[13]) {
            propCell = reinterpret_cast<GetPropFn>(vt[13])(props, keyHash);
        }
    }

    if (!propCell) {
        if (g_api) {
            g_api->Log("PlayerPropertyMatches: Test: property not found req=%p player=%p", self, player);
        }
        return 1;
    }

    unsigned int result = 0;
    const uint32_t flags = reinterpret_cast<uint32_t*>(propCell)[5];
    const unsigned type = propCell[0];
    LONG mask = 3145774;
    if ((flags & 0x20000000u) != 0 || type > 0x15u || !_bittest(&mask, static_cast<LONG>(type))) {
        result = minimum > 0 ? 1u : 0u;
    } else {
        // PropertyMinimum is a signed threshold.  The container stores the
        // payload in a 32-bit cell, but converting the negative threshold to
        // uint32_t would turn -1 into 4294967295 and make almost every value
        // appear below the threshold.
        const int32_t value = static_cast<int32_t>(reinterpret_cast<uint32_t*>(propCell)[2]);
        result = static_cast<unsigned int>(value < static_cast<int32_t>(minimum));
    }
    if (g_api) {
        g_api->Log(
            "PlayerPropertyMatches: Test result req=%p key=%08X minimum=%d type=%u value=%d state=%u",
            self,
            keyHash,
            minimum,
            type,
            reinterpret_cast<uint32_t*>(propCell)[2],
            result);
    }
    return static_cast<int>(result);
}


char __fastcall Detour_PlotOnInitialize(void* req, int64_t* params, int64_t ctx) {
    if (g_customCreateActive
        || IsCustomRequirement(req)) {
        return Hook_OnInitialize(req, params, ctx);
    }
    return g_origPlotOnInitialize ? g_origPlotOnInitialize(req, params, ctx) : 0;
}


int __fastcall Detour_PlotTest(void* req, void* a2, void* a3) {
    if (g_customCreateActive
        || IsCustomRequirement(req)) {
        return Hook_Test(req, a2, a3);
    }
    return g_origPlotTest ? g_origPlotTest(req, a2, a3) : 3;
}


const char* __fastcall Hook_GetTypeName(void* /*self*/) {
    return kRequirementTypeName;
}


int __fastcall Hook_ValidateDefinition(void* /*self*/, const void* definitionSharedPtr) {
    void* definition = definitionSharedPtr
        ? *reinterpret_cast<void* const*>(definitionSharedPtr)
        : nullptr;
    return GetDefinitionTypeHash(definition) == g_requirementTypeHash ? 0 : 4;
}

struct CustomCreateScope {
    bool previousActive;
    CustomCreateScope() : previousActive(g_customCreateActive) {
        g_customCreateActive = true;
    }
    ~CustomCreateScope() {
        g_customCreateActive = previousActive;
    }
};

void* __fastcall Hook_Create(void* self, void* outSharedPtr, const void* params) {
    if (g_api) {
        g_api->Log("PlayerPropertyMatches: Create begin factory=%p out=%p params=%p", self, outSharedPtr, params);
    }
    if (!g_createPlotFactory) {
        *reinterpret_cast<void**>(outSharedPtr) = nullptr;
        *(reinterpret_cast<void**>(outSharedPtr) + 1) = nullptr;
        return outSharedPtr;
    }
    // The native Plot factory owns allocation, shared_ptr construction and
    // destruction. The detours use this thread-local scope to recognize the
    // custom instance while native Initialize is still in progress.
    CustomCreateScope scope;
    g_createPlotFactory(self, outSharedPtr, params);
    void* requirement = *reinterpret_cast<void**>(outSharedPtr);
    if (g_api) {
        g_api->Log(
            "PlayerPropertyMatches: Create: native plot instance req=%p custom=%u",
            requirement, static_cast<unsigned>(IsCustomRequirement(requirement)));
    }
    return outSharedPtr;
}

bool BuildVtables(HookAPI* api) {
    if (!g_plotFactoryVftable) {
        return false;
    }
    auto** srcFactory = reinterpret_cast<void**>(g_plotFactoryVftable);
    g_ourFactoryVftable[-1] = srcFactory[-1];
    std::memcpy(g_ourFactoryVftable, srcFactory, 6 * sizeof(void*));

    int createSlot = FindSlot(srcFactory, reinterpret_cast<void*>(g_createPlotFactory), 6);
    int typeNameSlot = FindSlot(srcFactory, g_getTypeNamePlot, 6);
    int validateSlot = FindSlot(srcFactory, g_validateDefinitionPlot, 6);
    if (createSlot < 0) {
        createSlot = kFactoryCreateSlot;
    }
    if (typeNameSlot < 0) {
        typeNameSlot = kFactoryGetTypeNameSlot;
    }
    if (validateSlot < 0) {
        validateSlot = kFactoryValidateDefinitionSlot;
    }

    api->Log("PlayerPropertyMatches factory slots: Create=%d GetTypeName=%d Validate=%d",
        createSlot, typeNameSlot, validateSlot);

    g_ourFactoryVftable[createSlot] = reinterpret_cast<void*>(&Hook_Create);
    g_ourFactoryVftable[typeNameSlot] = reinterpret_cast<void*>(&Hook_GetTypeName);
    g_ourFactoryVftable[validateSlot] = reinterpret_cast<void*>(&Hook_ValidateDefinition);

    g_vtablesReady = true;
    return true;
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

bool PushFactory(HookAPI* api) {
    if (!g_getTypes || !g_vtablesReady) {
        return false;
    }
    auto* types = reinterpret_cast<StdVectorPtr*>(g_getTypes());
    if (!types) {
        return false;
    }

    if (FindFactoryByTypeName(kRequirementTypeName)) {
        api->Log("PlayerPropertyMatches: factory already present, skip push");
        return true;
    }

    void* mem = g_opNew ? g_opNew(8) : operator new(8, std::nothrow);
    if (!mem) {
        return false;
    }
    std::memset(mem, 0, 8);
    auto* factory = static_cast<RequirementFactoryObject*>(mem);
    factory->vftable = g_ourFactoryVftable;

    if (types->end == types->capacity) {
        api->Log("PlayerPropertyMatches: Registry::GetTypes() full");
        return false;
    }

    *types->end = factory;
    ++types->end;
    api->Log("PlayerPropertyMatches: registered %s factory=%p count=%llu",
        kRequirementTypeName,
        factory,
        static_cast<unsigned long long>(types->end - types->begin));
    return true;
}


}  // namespace

bool NewRequirements_RegisterPlayerProperty(HookAPI* api) {
    if (!HookAPI_IsCompatible(api)) {
        return false;
    }
    g_api = api;
    g_getTypes = reinterpret_cast<GetTypesFn>(ResolveSpec(api, kSymGetTypes));
    g_createPlotFactory = reinterpret_cast<FactoryCreateFn>(ResolveSpec(api, kSymCreatePlotFactory));
    g_getTypeNamePlot = ResolveSpec(api, kSymGetTypeNamePlotFactory);
    g_validateDefinitionPlot = ResolveSpec(api, kSymValidatePlotFactory);
    g_onInitializePlot = ResolveSpec(api, kSymInitPlot);
    g_testPlot = ResolveSpec(api, kSymTestPlot);
    g_plotFactoryVftable = ResolveSpec(api, kSymPlotFactoryVftable);
    g_plotPropertyRequirementVftable = ResolveSpec(api, kSymPlotRequirementVftable);
    g_fautoEdit = reinterpret_cast<FAutoEditFn>(ResolveSpec(api, kSymFAutoEdit));
    g_opNew = reinterpret_cast<OpNewFn>(ResolveSpec(api, kSymOpNew));
    g_gameEdit = reinterpret_cast<GameEditFn>(ResolveSpec(api, kSymGameEdit));
    g_makeHash = reinterpret_cast<MakeHashFn>(ResolveSpec(api, kSymMakeHash));
    g_setState = reinterpret_cast<SetStateFn>(ResolveSpec(api, kSymSetState));
    g_argProcessorVftable = ResolveSpec(api, kSymArgProcessorVftable);
    void* playerPropDispatch = ResolveSpec(api, kSymPlayerPropDispatch);
    void* luaPlayerSetProperty = ResolveSpec(api, kSymLuaPlayerSetProperty);
    g_luaGetPlayerInstance =
        reinterpret_cast<LuaGetPlayerInstanceFn>(ResolveSpec(api, kSymLuaGetPlayerInstance));
    g_luaIsNumber = reinterpret_cast<LuaPredicateFn>(ResolveSpec(api, kSymLuaIsNumber));
    g_luaIsString = reinterpret_cast<LuaPredicateFn>(ResolveSpec(api, kSymLuaIsString));
    g_luaToInteger = reinterpret_cast<LuaToIntegerFn>(ResolveSpec(api, kSymLuaToInteger));
    g_luaToString = reinterpret_cast<LuaToStringFn>(ResolveSpec(api, kSymLuaToString));

    if (!LoadGuidFromImage(api, kSymIPlayerInterfaceGuid, g_iPlayerInterfaceGuid)) {
        api->Log("PlayerPropertyMatches: failed to load subject interface GUID");
        return false;
    }
    if (g_makeHash) {
        g_requirementTypeHash = g_makeHash(kRequirementTypeName);
    }

    if (g_plotPropertyRequirementVftable && g_testPlot) {
        g_requirementTestSlot = FindSlot(
            reinterpret_cast<void**>(g_plotPropertyRequirementVftable), g_testPlot, 32);
    }

    if (!g_createPlotFactory || !g_plotFactoryVftable || !g_getTypes
        || !g_getTypeNamePlot || !g_validateDefinitionPlot || !g_onInitializePlot
        || !g_testPlot || !g_makeHash || !g_requirementTypeHash || !g_fautoEdit
        || !g_setState || !g_gameEdit || !playerPropDispatch || !g_argProcessorVftable
        || !luaPlayerSetProperty || !g_luaGetPlayerInstance
        || !g_luaIsNumber || !g_luaIsString || !g_luaToInteger || !g_luaToString
        || !g_plotPropertyRequirementVftable || g_requirementTestSlot < 0) {
        api->Log("PlayerPropertyMatches: missing critical symbols");
        return false;
    }
    if (!BuildVtables(api)) {
        api->Log("PlayerPropertyMatches: failed to build factory vtable");
        return false;
    }

    if (!api->InstallHook(
            g_onInitializePlot,
            reinterpret_cast<void*>(&Detour_PlotOnInitialize),
            reinterpret_cast<void**>(&g_origPlotOnInitialize))) {
        api->Log("PlayerPropertyMatches: failed to install Detour_PlotOnInitialize");
        return false;
    }
    api->Log("PlayerPropertyMatches: Detour_PlotOnInitialize installed at %p", g_onInitializePlot);
    if (!api->InstallHook(
            g_testPlot,
            reinterpret_cast<void*>(&Detour_PlotTest),
            reinterpret_cast<void**>(&g_origPlotTest))) {
        api->Log("PlayerPropertyMatches: failed to install Detour_PlotTest");
        return false;
    }
    api->Log("PlayerPropertyMatches: Detour_PlotTest installed at %p", g_testPlot);
    if (!api->InstallHook(
            playerPropDispatch,
            reinterpret_cast<void*>(&Hook_PlayerPropDispatch),
            reinterpret_cast<void**>(&g_origPlayerPropDispatch))) {
        api->Log("PlayerPropertyMatches: failed to install Hook_PlayerPropDispatch");
        return false;
    }
    api->Log("PlayerPropertyMatches: Hook_PlayerPropDispatch installed at %p", playerPropDispatch);
    if (!api->InstallHook(
            luaPlayerSetProperty,
            reinterpret_cast<void*>(&Hook_LuaPlayerSetProperty),
            reinterpret_cast<void**>(&g_origLuaPlayerSetProperty))) {
        api->Log("PlayerPropertyMatches: failed to install Hook_LuaPlayerSetProperty");
        return false;
    }
    api->Log("PlayerPropertyMatches: Hook_LuaPlayerSetProperty installed at %p", luaPlayerSetProperty);
    const bool registered = PushFactory(api);
    api->Log("PlayerPropertyMatches: registration %s", registered ? "OK" : "FAILED");
    return registered;
}
