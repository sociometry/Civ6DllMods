#pragma once

struct HookAPI;

// ModifierManagerAPI —— 修饰符挂接与摘除接口（APIExtension 分类）
//
// 源：CivilizationVI_CommunityExtension-1.3.0-beta 的 Unit.cpp
// （lAttachModifierByID / lDetachModifierByID，经 PushMethods 注入到 IUnit 上），
// 迁移为 Civ6DllLoader 插件形态：
//
//   unit:AttachModifierByID(modifierID) -> number
//       挂一层字符串 ID 的修饰符并激活；成功返回该层数字索引，失败返回 -1。
//       实例方法注入到 GameCore::Lua::IUnit（PushLuaMethods 标准路径），
//       用法与 CE 版一致。
//
//   ModifierManager.DetachModifierByID(owner, modifierID[, count]) -> number
//       摘除该 owner 上 ID 匹配的修饰符，count 缺省为 1；返回实际摘掉的层数。
//       owner 可以是 Unit / City / Player / District 四种 Lua 对象。
//
//   ModifierManager.GetCount(owner, modifierID) -> number
//       该 owner 上当前挂着的同名修饰符层数。
//
// Detach/GetCount 不依赖本插件的台账：直接用引擎的 MakeObject(owner) 把实例包成
// IModifierObject，再调 ModifierSystem::GetModifiersOwnedBy 拿该 owner 自有的全部
// 修饰符，逐个比对 ModifierDefinition 的 ID。因此引擎自己挂的、别的 mod 挂的、
// 上一局存档带过来的层都能摘。摘除动作是 ClearModifier(map 项) + IModifier::Deactivate
// （撤销属性加成）——只做前者会让 maxMoves 之类的加成留在单位身上。
//
// owner 类型判定走引擎的 TypeName 字符串（"Unit"/"City"/"Player"/"District"），
// 与原生 ScopedInstance 的校验一致。
void ModifierManagerAPI_Init(HookAPI* api);
void ModifierManagerAPI_Shutdown();
