#pragma once

struct HookAPI;

// ModConfig —— 全局 Lua 配置接口（Feature 分类，Global 侧）
//
// 给 mod 作者一个开箱即用的键值持久化接口：ModConfig.Get/Set/Load/Save/...，
// 数据落在 Documents\My Games\Sid Meier's Civilization VI\ModConfigs\<名字>.json。
//
// Global 侧通过 RegisterGlobalScript 注入到**引擎主 Lua state**：进程级、换局不失效，
// 因此主菜单 UI（FrontEnd）与对局内的脚本都能用同一份配置。
//
// 详细设计与 Lua 侧 API 见 ModConfigCore.h 顶部注释。
void ModConfig_Init(HookAPI* api);
void ModConfig_Shutdown();
