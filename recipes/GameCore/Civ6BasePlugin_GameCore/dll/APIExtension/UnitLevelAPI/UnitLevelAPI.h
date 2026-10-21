#pragma once

struct HookAPI;

// UnitLevelAPI —— 给 Unit 的 Experience 补一个 SetLevel 接口（APIExtension 分类）
//
// 引擎现状：
//   * 单位经验相关接口都挂在 Lua 的 "Unit::Experience" 表下：
//       unit:GetExperience():GetLevel()
//       unit:GetExperience():GetExperiencePoints()
//       unit:GetExperience():GetExperienceForNextLevel()
//       unit:GetExperience():ChangeExperience(n)
//       unit:GetExperience():SetPromotion(id) / SetVeteranName(name) / ...
//     唯独没有 SetLevel（写等级）。
//   * 引擎内部有 GameCore::Unit::Experience::SetLevel(int)（RVA 0x3974A0），
//     但没有暴露到 Lua。
//
// 本模块把 SetLevel 接到同一张表下，调用形式与同族接口一致：
//     unit:GetExperience():SetLevel(n)   -> boolean
//
// ---------------------------------------------------------------------------
// 挂载机制（引擎做法，本模块照搬）
// ---------------------------------------------------------------------------
// 引擎的 ScopedInstance<IUnitExperience,Unit::Experience>::Push（RVA 0x71C330）：
//   lua_pushlstring("Unit::Experience")
//   getfield(...)                 // 取/建该全局表
//   createTable / __instances / __mode="v"
//   IUnitExperience::PushMethods(L, idx)    // 往表里塞方法
//
// 而 PushMethods（RVA 0x720030）逐个方法就是：
//   hks_pushnamedcclosure(luaFn, "lXxx")     // 内部名
//   hksi_lua_setfield(L, idx, "Xxx")         // 暴露名
//
// 框架侧对应 HookAPI 的 PushLuaMethods：需给出完整的 PushMethods 符号名，
// 原 PushMethods 由 Loader 自动调用，因此只需给出符号、签名与追加的方法。
//
// ---------------------------------------------------------------------------
// 依赖面压到最小
// ---------------------------------------------------------------------------
// 只解析 2 个官方函数：
//   1) Experience::SetLevel       有完整 mangled 名，符号解析最稳；
//   2) Lua Experience::GetInstance 从 Lua 栈取 Experience*，绕不开。
void UnitLevelAPI_Init(HookAPI* api);
void UnitLevelAPI_Shutdown();