#pragma once

struct HookAPI;

// GreatWorkYieldStack —— 巨作产出加成不叠加修复（BugFix 分类）
//
// 源：CivilizationVI_CommunityExtension-1.3.0-beta 的 Main.cpp（GET_BB_OFFSET + Hook_BB）。
//
// 缺陷：GameCore::City::Culture::GetGreatWorkYieldChange(cityCulture,
// greatWorkObjectType, yieldType) 在 City::Culture 的加成表（+0x68..+0x70，
// 每项 {objectType, yieldType, amount, pad} 共 16 字节）里找到第一条匹配就立即
// 返回其 amount。同一（物件类型，产出类型）组合有多条来源时只算一条——
// 巨作产出加成无法叠加。
//
// 修法：hook 改为遍历整表把所有匹配条目的正数 amount 求和返回。
// 唯一调用方 City::Buildings::GetBuildingYieldFromGreatWorks 因此恢复叠加。
void GreatWorkYieldStack_Init(HookAPI* api);
void GreatWorkYieldStack_Shutdown();
