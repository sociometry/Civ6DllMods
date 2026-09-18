#pragma once

#include "HookAPI.h"

// Each function independently installs its hooks and registers its factory.
// Call once per plugin load; comment out either call in dllmain.cpp to disable it.
bool NewRequirements_RegisterPlayerProperty(HookAPI* api);
bool NewRequirements_RegisterPlotRoute(HookAPI* api);

// Called before engine GameCore teardown, while native destructors are available.
void NewRequirements_UnloadPlayerProperty();
void NewRequirements_UnloadPlotRoute();
