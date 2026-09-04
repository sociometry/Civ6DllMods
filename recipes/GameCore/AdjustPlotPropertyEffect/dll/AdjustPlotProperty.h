#pragma once

#include "HookAPI.h"

// Registers EFFECT_ADJUST_PLOT_PROPERTY into GameEffects::Registry.
// Returns true if factory injection succeeded.
bool AdjustPlotPropertyEffect_Register(HookAPI* api);
