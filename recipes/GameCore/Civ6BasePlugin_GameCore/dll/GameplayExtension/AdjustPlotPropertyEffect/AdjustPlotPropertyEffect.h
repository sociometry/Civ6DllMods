#pragma once

#include "HookAPI.h"

// EFFECT_ADJUST_PLOT_PROPERTY —— 克隆 AdjustUnitProperty 的工厂与效果虚表，
// 把自定义 effect 挂进 GameEffects::Registry。地块属性经 IPlotInterface ±delta。
void AdjustPlotPropertyEffect_Init(HookAPI* api);
void AdjustPlotPropertyEffect_Shutdown();
