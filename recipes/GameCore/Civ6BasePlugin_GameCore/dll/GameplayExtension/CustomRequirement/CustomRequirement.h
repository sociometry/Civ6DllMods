#pragma once

// CustomRequirement —— 往 GameEffects 注册表注入两个自定义 Requirement。
//
// 每个 Requirement 一个 .cpp，各自持有独立的匿名命名空间与状态；地址解析、
// shared_ptr、Registry 等辅助代码有意重复，方便单独阅读和复制。两个模块互不调用，
// 任一模块失败不影响另一个（各自装钩、各自记日志）。
//
// 与 Loader 的插件约定一致：Init 里做的登记由 Loader 按 owner 统一回收，
// Shutdown 只断引用（避免 detour 跳回已卸载模块）。

#include "HookAPI.h"

// REQUIREMENT_PLAYER_PROPERTY_MATCHES —— 按玩家动态属性阈值匹配。
// 装 4 个钩：PlotProperty OnInitialize / Test、玩家属性 Dispatch、Lua SetProperty。
void CustomRequirement_InitPlayerProperty(HookAPI* api);
void CustomRequirement_ShutdownPlayerProperty();

// REQUIREMENT_PLOT_ROUTE_TYPE_MATCHES —— 按地块道路类型匹配。
// 装 3 个钩：Terrain TestPlot、PlotRequirement Test、SetRouteType。
void CustomRequirement_InitPlotRoute(HookAPI* api);
void CustomRequirement_ShutdownPlotRoute();