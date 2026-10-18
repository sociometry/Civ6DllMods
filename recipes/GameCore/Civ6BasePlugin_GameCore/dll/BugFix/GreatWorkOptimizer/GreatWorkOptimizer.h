#pragma once

struct HookAPI;

// GreatWorkOptimizer —— CultureAi_GreatWorks::Execute 巨作分配优化（BugFix 分类）
//
// 源：CivilizationVI_CommunityExtension-1.3.0-beta 的 GreatWorkOptimizer.cpp，
// 按 Civ6DllLoader HookAPI 规范移植。
//
// 固定行为，无运行时开关/消息通道：
//   * Execute 结果缓存：巨作集合（各城槽位分布）未变时跳过 O(G^3) 全量重算；
//   * bounded_knapsack 容量钳制：capacity > 3 与 3 等价（最大组合数为 3），剪掉冗余 DP 列；
//   * 人类玩家整次跳过 Execute：文化 AI 重算对人类无意义（巨作摆放由玩家手动控制）。
//
// 生命周期：本工程是 **GameCore 插件**，OnLoad 时 GameCore 早已加载完毕——
// 「等 GameCore 就绪」的语义由框架本身满足，因此这里没有（也不再有）Ready/Teardown
// 回调：OnLoad 直接解析符号 + 装钩，退局时 Loader 按 owner 统一回滚。
void GreatWorkOptimizer_Init(HookAPI* api);
void GreatWorkOptimizer_Shutdown();
