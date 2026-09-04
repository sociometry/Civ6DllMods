#include "pch.h"
#include "AdjustPlotProperty.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <new>

// ---------------------------------------------------------------------------
// EFFECT_ADJUST_PLOT_PROPERTY
//
// Clone AdjustUnitProperty factory/effect, then:
// - factory GetTypeName / Create -> ours
// - effect IModifierEffect Apply/Remove (UnitEffect slots) -> ours
// Apply resolves Plot via IPlotInterface (same pattern as AdjustPlotYield /
// PlotPropertyMatches), then EditDynamicProperties + numeric Key/Amount.
// ---------------------------------------------------------------------------

namespace {

constexpr const char* kEffectTypeName = "EFFECT_ADJUST_PLOT_PROPERTY";

// IPlotInterface GUID == xmmword_180A9A508 (OLD: 180A99158)
alignas(16) const uint8_t kIPlotInterfaceGuid[16] = {
    0x8B, 0xA8, 0x53, 0xB6, 0x1D, 0x12, 0xAF, 0x43,
    0x89, 0x17, 0x2E, 0xAD, 0xA2, 0xD3, 0x9F, 0x84,
};

// Verified AdjustUnitProperty effect vftable layout (NEW 0xA9E440):
//   [5] UnitEffect::Apply(shared_ptr, int)   — game entry
//   [6] UnitEffect::Remove(shared_ptr, int)
//   [12] AdjustUnitProperty::Apply(Unit*)
//   [13] AdjustUnitProperty::Remove(Unit*)
// Factory (NEW 0xA92560): [2] GetTypeName, [5] Create
constexpr int kFactoryGetTypeNameSlot = 2;
constexpr int kFactoryCreateSlot = 5;
constexpr int kEffectApplySharedSlot = 5;
constexpr int kEffectRemoveSharedSlot = 6;

struct SymbolSpec {
    const char* name;
    uint32_t oldRva;
    uint32_t newRvaHint;
};

// clang-format off
const SymbolSpec kSymEditDynamicProperties{
    "?EditDynamicProperties@Instance@Plot@Map@GameCore@@UEAAAEAVTypedVariantKeyedContainer@Data@@XZ",
    0x5E310, 0x5EF50};
const SymbolSpec kSymGetDynamicProperties{
    "?GetDynamicProperties@Instance@Plot@Map@GameCore@@UEBAAEBVTypedVariantKeyedContainer@Data@@XZ",
    0x5E620, 0x5F270};
const SymbolSpec kSymMakeHash{
    "?MakeHash@Utilities@GameCore@@YAIPEBD@Z",
    0x605600, 0x606270};
const SymbolSpec kSymGetTypes{
    "?GetTypes@?$Registry@VIModifierEffectFactory@GameEffects@@@GameEffects@@SAAEAV?$vector@V?$unique_ptr@VIModifierEffectFactory@GameEffects@@U?$default_delete@VIModifierEffectFactory@GameEffects@@@std@@@std@@V?$allocator@V?$unique_ptr@VIModifierEffectFactory@GameEffects@@U?$default_delete@VIModifierEffectFactory@GameEffects@@@std@@@std@@@2@@std@@XZ",
    0x95A950, 0x95B0C0};
const SymbolSpec kSymInitUnitProp{
    "?Initialize@AdjustUnitProperty@Effects@GameEffects@@UEAA_NAEBUEffectParameters@3@@Z",
    0x8AAEC0, 0x810AA0};
const SymbolSpec kSymCreateUnitFactory{
    "?Create@?$EffectFactory@VAdjustUnitProperty@Effects@GameEffects@@@Factories@GameEffects@@UEAA?AV?$shared_ptr@VIModifierEffect@GameEffects@@@std@@AEBUEffectParameters@3@@Z",
    0x8888C0, 0x8A3850};
const SymbolSpec kSymGetTypeNameUnitFactory{
    "?GetTypeName@?$EffectFactory@VAdjustUnitProperty@Effects@GameEffects@@@Factories@GameEffects@@UEBAPEBDXZ",
    0x8872E0, 0x8A1FA0};
const SymbolSpec kSymApplyUnitProp{
    "?Apply@AdjustUnitProperty@Effects@GameEffects@@UEAA_NPEAVInstance@Unit@GameCore@@@Z",
    0x8C06C0, 0x8D9D00};
const SymbolSpec kSymRemoveUnitProp{
    "?Remove@AdjustUnitProperty@Effects@GameEffects@@UEAA_NPEAVInstance@Unit@GameCore@@@Z",
    0x8C11D0, 0x8DA950};
const SymbolSpec kSymUnitFactoryVftable{
    "??_7?$EffectFactory@VAdjustUnitProperty@Effects@GameEffects@@@Factories@GameEffects@@6B@",
    0xA8C238, 0xA92560};
const SymbolSpec kSymUnitEffectVftable{
    "??_7AdjustUnitProperty@Effects@GameEffects@@6B@",
    0xA98268, 0xA9E440};
const SymbolSpec kSymGameEdit{
    "?Edit@Instance@Game@GameCore@@SAAEAV123@XZ",
    0x950D10, 0x951550};
const SymbolSpec kSymPlotCoordDispatch{
    "?Dispatch@?$ProtoSignal@$$A6AXUPlotCoord@GameCore@@IH@ZU?$CollectorDefault@X@SignalBase@Message@2@@SignalBase@Message@GameCore@@QEAAXUPlotCoord@4@IH@Z",
    0x19F70, 0x1A460};
const SymbolSpec kSymOpNew{
    "??2@YAPEAX_K@Z",
    0x9901A0, 0x990900};
// clang-format on

HookAPI* g_api = nullptr;

using MakeHashFn = uint32_t(__cdecl*)(const char*);
using GetTypesFn = void*(__cdecl*)();
using FactoryCreateFn = void*(__fastcall*)(void* self, void* outSharedPtr, const void* params);
using EditDynamicPropertiesFn = void*(__fastcall*)(void* plot);
using GameEditFn = void*(__cdecl*)();
using PlotCoordDispatchFn = void(__fastcall*)(void* signal, void* plotCoord, unsigned int keyHash, int value);
using OpNewFn = void*(__cdecl*)(size_t);

MakeHashFn g_makeHash = nullptr;
GetTypesFn g_getTypes = nullptr;
FactoryCreateFn g_createUnitFactory = nullptr;
EditDynamicPropertiesFn g_editDynamicProperties = nullptr;
GameEditFn g_gameEdit = nullptr;
PlotCoordDispatchFn g_plotCoordDispatch = nullptr;
OpNewFn g_opNew = nullptr;

void* g_unitFactoryVftable = nullptr;
void* g_unitEffectVftable = nullptr;
void* g_applyUnit = nullptr;
void* g_removeUnit = nullptr;
void* g_getTypeNameUnit = nullptr;

void* g_ourFactoryVftable[32]{};
void* g_ourEffectVftable[32]{};
bool g_vtablesReady = false;

struct FactoryObject {
    void** vftable;
    uint32_t typeHash;
};

struct EffectObject {
    void** vftable;
    uint32_t keyHash;  // +8
    int amount;        // +12
};

struct StdVectorPtr {
    void** begin;
    void** end;
    void** capacity;
};

void* ResolveSpec(HookAPI* api, const SymbolSpec& spec) {
    if (!api) {
        return nullptr;
    }
    void* addr = api->ResolveByName(kHookImageGameCoreXP2, spec.name);
    if (addr) {
        return addr;
    }
    if (spec.newRvaHint) {
        return api->ResolveByRva(
            kHookImageGameCoreXP2, static_cast<uintptr_t>(spec.newRvaHint), 1);
    }
    return api->ResolveByRva(
        kHookImageGameCoreXP2, static_cast<uintptr_t>(spec.oldRva), 1);
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

void* QueryInterface(void* modifierObject, const void* guid, bool required) {
    if (!modifierObject) {
        return nullptr;
    }
    auto** vt = *reinterpret_cast<void***>(modifierObject);
    using QiFn = void*(__fastcall*)(void*, const void*, bool);
    auto qi = reinterpret_cast<QiFn>(vt[2]);  // +0x10
    return qi(modifierObject, guid, required);
}

void* GetPlotFromIPlotInterface(void* ifacePlus8) {
    if (!ifacePlus8 || ifacePlus8 == reinterpret_cast<void*>(8)) {
        return nullptr;
    }
    // QueryInterface returns subobject+8; call (iface-8)->vfunc[0]
    void* iface = static_cast<char*>(ifacePlus8) - 8;
    auto** vt = *reinterpret_cast<void***>(iface);
    using GetPlotFn = void*(__fastcall*)(void*);
    return reinterpret_cast<GetPlotFn>(vt[0])(iface);
}

void* ResolvePlotFromModifierObject(void* modifierObject) {
    if (!modifierObject) {
        return nullptr;
    }
    void* iface = QueryInterface(modifierObject, kIPlotInterfaceGuid, true);
    if (void* plot = GetPlotFromIPlotInterface(iface)) {
        return plot;
    }
    // Heuristic: subject may already be Plot::Instance* (EditDynamicProperties @ vt[12]).
    auto** vt = *reinterpret_cast<void***>(modifierObject);
    if (vt && g_editDynamicProperties && vt[12] == reinterpret_cast<void*>(g_editDynamicProperties)) {
        return modifierObject;
    }
    return nullptr;
}

int ReadNumericProperty(void* props, uint32_t keyHash) {
    if (!props) {
        return 0;
    }
    auto** vt = *reinterpret_cast<void***>(props);
    using GetPropFn = uint16_t*(__fastcall*)(void*, uint32_t);
    uint16_t* prop = nullptr;
    // PlotPropertyMatches uses +0x60; Adjust*Property uses +0x68.
    if (vt[12]) {
        prop = reinterpret_cast<GetPropFn>(vt[12])(props, keyHash);
    }
    if (!prop && vt[13]) {
        prop = reinterpret_cast<GetPropFn>(vt[13])(props, keyHash);
    }
    if (!prop) {
        return 0;
    }
    const uint32_t flags = reinterpret_cast<uint32_t*>(prop)[5];
    const unsigned type = prop[0];
    LONG mask = 3145774;
    if ((flags & 0x20000000u) != 0 || type > 0x15u || !_bittest(&mask, static_cast<LONG>(type))) {
        return 0;
    }
    return static_cast<int>(reinterpret_cast<uint32_t*>(prop)[2]);
}

void WriteNumericProperty(void* props, uint32_t keyHash, int newValue) {
    if (!props) {
        return;
    }
    auto** vt = *reinterpret_cast<void***>(props);
    using SetPropFn = void(__fastcall*)(void*, uint32_t, uint32_t);
    if (vt[38]) {  // +0x130
        reinterpret_cast<SetPropFn>(vt[38])(props, keyHash, static_cast<uint32_t>(newValue));
    }
}

void DispatchPlotPropertyChanged(void* plot, uint32_t keyHash, int delta) {
    if (!plot || !g_gameEdit || !g_plotCoordDispatch) {
        return;
    }
    // plot+40/+42 = x/y (int16); lSetProperty packs PlotCoord then Dispatch.
    const auto* words = reinterpret_cast<int16_t*>(plot);
    int coord[2]{words[20], words[21]};
    void* game = g_gameEdit();
    if (!game) {
        return;
    }
    // *(Game::Edit()+0x280) + 0x1140  (== +640 / +4416 in lSetProperty)
    void* signalBase = *reinterpret_cast<void**>(static_cast<char*>(game) + 0x280);
    if (!signalBase) {
        return;
    }
    void* signal = static_cast<char*>(signalBase) + 0x1140;
    g_plotCoordDispatch(signal, coord, keyHash, delta);
}

bool AdjustPlotPropertyDelta(void* modifierObject, uint32_t keyHash, int delta) {
    void* plot = ResolvePlotFromModifierObject(modifierObject);
    if (!plot) {
        if (g_api) {
            g_api->Log("AdjustPlotProperty: no Plot from subject=%p", modifierObject);
        }
        return false;
    }

    void* props = nullptr;
    if (g_editDynamicProperties) {
        props = g_editDynamicProperties(plot);
    } else {
        auto** vt = *reinterpret_cast<void***>(plot);
        using VFn = void*(__fastcall*)(void*);
        props = reinterpret_cast<VFn>(vt[12])(plot);  // +0x60 EditDynamicProperties
    }
    if (!props) {
        return false;
    }

    const int oldValue = ReadNumericProperty(props, keyHash);
    WriteNumericProperty(props, keyHash, oldValue + delta);
    DispatchPlotPropertyChanged(plot, keyHash, delta);
    if (g_api) {
        g_api->Log("AdjustPlotProperty: plot=%p key=%u %d -> %d", plot, keyHash, oldValue, oldValue + delta);
    }
    return true;
}

// IModifierEffect entry: Apply(this, shared_ptr<IModifierObject>&, int)
char __fastcall Hook_Apply(EffectObject* self, void** subjectSharedPtr, int /*run*/) {
    if (!self || !subjectSharedPtr || !*subjectSharedPtr) {
        return 0;
    }
    return AdjustPlotPropertyDelta(*subjectSharedPtr, self->keyHash, self->amount) ? 1 : 0;
}

char __fastcall Hook_Remove(EffectObject* self, void** subjectSharedPtr, int /*run*/) {
    if (!self || !subjectSharedPtr || !*subjectSharedPtr) {
        return 0;
    }
    return AdjustPlotPropertyDelta(*subjectSharedPtr, self->keyHash, -self->amount) ? 1 : 0;
}

const char* __fastcall Hook_GetTypeName(void* /*self*/) {
    return kEffectTypeName;
}

void* __fastcall Hook_Create(void* self, void* outSharedPtr, const void* params) {
    if (!g_createUnitFactory) {
        *reinterpret_cast<void**>(outSharedPtr) = nullptr;
        *(reinterpret_cast<void**>(outSharedPtr) + 1) = nullptr;
        return outSharedPtr;
    }
    // Reuse Unit factory allocator + Initialize(Key/Amount), then swap effect vf.
    g_createUnitFactory(self, outSharedPtr, params);
    void* effect = *reinterpret_cast<void**>(outSharedPtr);
    if (effect && g_vtablesReady) {
        *reinterpret_cast<void***>(effect) = g_ourEffectVftable;
    }
    return outSharedPtr;
}

bool BuildVtables(HookAPI* api) {
    if (!g_unitFactoryVftable || !g_unitEffectVftable) {
        return false;
    }
    auto** srcFactory = reinterpret_cast<void**>(g_unitFactoryVftable);
    auto** srcEffect = reinterpret_cast<void**>(g_unitEffectVftable);
    std::memcpy(g_ourFactoryVftable, srcFactory, sizeof(g_ourFactoryVftable));
    std::memcpy(g_ourEffectVftable, srcEffect, sizeof(g_ourEffectVftable));

    int createSlot = FindSlot(srcFactory, reinterpret_cast<void*>(g_createUnitFactory), 32);
    int typeNameSlot = FindSlot(srcFactory, g_getTypeNameUnit, 32);
    if (createSlot < 0) {
        createSlot = kFactoryCreateSlot;
    }
    if (typeNameSlot < 0) {
        typeNameSlot = kFactoryGetTypeNameSlot;
    }

    // Must patch UnitEffect shared_ptr Apply/Remove — not Apply(Unit*).
    const int applySlot = kEffectApplySharedSlot;
    const int removeSlot = kEffectRemoveSharedSlot;

    // Sanity: Unit* Apply should sit at 12 if vf is AdjustUnitProperty.
    const int unitApplySlot = FindSlot(srcEffect, g_applyUnit, 32);
    const int unitRemoveSlot = FindSlot(srcEffect, g_removeUnit, 32);

    api->Log("AdjustPlotProperty vtable slots: Create=%d GetTypeName=%d ApplyShared=%d RemoveShared=%d (UnitApply=%d UnitRemove=%d)",
        createSlot, typeNameSlot, applySlot, removeSlot, unitApplySlot, unitRemoveSlot);

    if (createSlot < 0 || typeNameSlot < 0) {
        return false;
    }
    if (unitApplySlot >= 0 && unitApplySlot != 12) {
        api->Log("AdjustPlotProperty: unexpected Unit Apply slot %d (expected 12) — check effect vf", unitApplySlot);
    }

    g_ourFactoryVftable[createSlot] = reinterpret_cast<void*>(&Hook_Create);
    g_ourFactoryVftable[typeNameSlot] = reinterpret_cast<void*>(&Hook_GetTypeName);
    g_ourEffectVftable[applySlot] = reinterpret_cast<void*>(&Hook_Apply);
    g_ourEffectVftable[removeSlot] = reinterpret_cast<void*>(&Hook_Remove);
    g_vtablesReady = true;
    return true;
}

bool FindFactoryInRegistry(uint32_t typeHash) {
    if (!g_getTypes) {
        return false;
    }
    auto* types = reinterpret_cast<StdVectorPtr*>(g_getTypes());
    if (!types || !types->begin || !types->end) {
        return false;
    }
    for (void** it = types->begin; it != types->end; ++it) {
        auto* factory = static_cast<FactoryObject*>(*it);
        if (!factory || !factory->vftable) {
            continue;
        }
        if (factory->typeHash == typeHash) {
            return true;
        }
        // Fallback: compare GetTypeName (slot 2)
        using GetNameFn = const char*(__fastcall*)(void*);
        auto* getName = reinterpret_cast<GetNameFn>(factory->vftable[kFactoryGetTypeNameSlot]);
        if (getName) {
            const char* name = getName(factory);
            if (name && std::strcmp(name, kEffectTypeName) == 0) {
                return true;
            }
        }
    }
    return false;
}

bool PushFactory(HookAPI* api) {
    if (!g_getTypes || !g_vtablesReady) {
        return false;
    }
    auto* types = reinterpret_cast<StdVectorPtr*>(g_getTypes());
    if (!types) {
        return false;
    }

    uint32_t typeHash = 0;
    if (g_makeHash) {
        typeHash = g_makeHash(kEffectTypeName);
    } else if (api->MakeHash) {
        typeHash = api->MakeHash(kEffectTypeName);
    } else {
        api->Log("AdjustPlotProperty: MakeHash unavailable");
        return false;
    }

    if (FindFactoryInRegistry(typeHash)) {
        api->Log("AdjustPlotProperty: factory already present (hash=0x%08X), skip push", typeHash);
        return true;
    }

    void* mem = nullptr;
    if (g_opNew) {
        mem = g_opNew(sizeof(FactoryObject));
    } else {
        mem = operator new(sizeof(FactoryObject), std::nothrow);
    }
    if (!mem) {
        return false;
    }
    std::memset(mem, 0, sizeof(FactoryObject));
    auto* factory = static_cast<FactoryObject*>(mem);
    factory->vftable = g_ourFactoryVftable;
    factory->typeHash = typeHash;

    if (types->end == types->capacity) {
        api->Log("AdjustPlotProperty: Registry::GetTypes() is full; cannot append factory safely");
        return false;
    }

    *types->end = factory;
    ++types->end;
    api->Log("AdjustPlotProperty: registered %s hash=0x%08X factory=%p count=%llu",
        kEffectTypeName,
        factory->typeHash,
        factory,
        static_cast<unsigned long long>(types->end - types->begin));
    return true;
}

bool EnsureFactory(HookAPI* api) {
    if (!api || !g_vtablesReady) {
        return false;
    }
    return PushFactory(api);
}

}  // namespace

bool AdjustPlotPropertyEffect_Register(HookAPI* api) {
    if (!api) {
        return false;
    }
    g_api = api;

    g_editDynamicProperties =
        reinterpret_cast<EditDynamicPropertiesFn>(ResolveSpec(api, kSymEditDynamicProperties));
    ResolveSpec(api, kSymGetDynamicProperties);
    g_makeHash = reinterpret_cast<MakeHashFn>(ResolveSpec(api, kSymMakeHash));
    g_getTypes = reinterpret_cast<GetTypesFn>(ResolveSpec(api, kSymGetTypes));
    ResolveSpec(api, kSymInitUnitProp);
    g_createUnitFactory = reinterpret_cast<FactoryCreateFn>(ResolveSpec(api, kSymCreateUnitFactory));
    g_getTypeNameUnit = ResolveSpec(api, kSymGetTypeNameUnitFactory);
    g_applyUnit = ResolveSpec(api, kSymApplyUnitProp);
    g_removeUnit = ResolveSpec(api, kSymRemoveUnitProp);
    g_unitFactoryVftable = ResolveSpec(api, kSymUnitFactoryVftable);
    g_unitEffectVftable = ResolveSpec(api, kSymUnitEffectVftable);
    g_gameEdit = reinterpret_cast<GameEditFn>(ResolveSpec(api, kSymGameEdit));
    g_plotCoordDispatch = reinterpret_cast<PlotCoordDispatchFn>(ResolveSpec(api, kSymPlotCoordDispatch));
    g_opNew = reinterpret_cast<OpNewFn>(ResolveSpec(api, kSymOpNew));

    api->Log("AdjustPlotProperty resolves:");
    api->Log("  EditDynamicProperties=%p MakeHash=%p GetTypes=%p OpNew=%p",
        g_editDynamicProperties, g_makeHash, g_getTypes, g_opNew);
    api->Log("  Unit.Create=%p Unit.Apply=%p Unit.Remove=%p", g_createUnitFactory, g_applyUnit, g_removeUnit);
    api->Log("  FactoryVF=%p EffectVF=%p GameEdit=%p Dispatch=%p",
        g_unitFactoryVftable, g_unitEffectVftable, g_gameEdit, g_plotCoordDispatch);

    if (!g_createUnitFactory || !g_unitFactoryVftable || !g_unitEffectVftable || !g_getTypes) {
        api->Log("AdjustPlotProperty: missing critical symbols");
        return false;
    }
    if (!BuildVtables(api)) {
        api->Log("AdjustPlotProperty: failed to build patched vtables");
        return false;
    }
    if (!PushFactory(api)) {
        api->Log("AdjustPlotProperty: failed to push factory into Registry::GetTypes()");
        return false;
    }
    return true;
}
