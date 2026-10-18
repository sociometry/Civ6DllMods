#pragma once

struct HookAPI;

// GreatWorkTypeLookup —— 巨作「名哈希 -> 类型索引」解析修复（BugFix 分类）
//
// 前身：BugFix/GreatWorkSlotLookup（源自 CivilizationVI_CommunityExtension-1.3.0-beta
// 的 Main.cpp Hook_S）；现扩展覆盖同一缺陷模式的两个函数：
//
//   Definition::Utility::NameToGreatWorkSlotType   （槽位类型，原版 9 种）
//   Definition::Utility::NameToGreatWorkObjectType （物件类型，原版 11 种）
//
// 缺陷：两者都把「类型名哈希 -> 枚举」写死成 switch，未命中返回 -1。模组往数据库新增
// GreatWorkSlotType / GreatWorkObjectType 后解析必败——AddGreatWorkSlots、
// IsValidObjectForSlot、AI 评估、AdjustExtraGreatWork(Slot|Object)* 效果等调用点对
// 新类型全部失效。
//
// 修法：hook 后先查数据驱动的定义集合解析器（Types::GetIndex 族），本地维护一张
// 「哈希 -> 索引」缓存表（首次解析成功入表，此后直接读表），未命中再回退原硬编码
// switch。原版类型行为不变。
//
// 生命周期：GameCore 插件，OnLoad 时 GameCore 已就绪；退局由 Loader 回滚钩子，
// 缓存表在 Shutdown 与每次 OnLoad 时清空（不同代之间模组列表可能变化）。
void GreatWorkTypeLookup_Init(HookAPI* api);
void GreatWorkTypeLookup_Shutdown();
