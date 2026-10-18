#pragma once

struct HookAPI;

// ModifierManagerAPI —— 单位修饰符挂接与摘除接口（APIExtension 分类）
//
// 源：CivilizationVI_CommunityExtension-1.3.0-beta 的 Unit.cpp
// （lAttachModifierByID / lDetachModifierByID，经 PushMethods 注入到 IUnit 上），
// 迁移为 Civ6DllLoader 插件形态：
//
//   unitInstance:AttachModifierByID(modifierID) -> number
//       挂一层字符串 ID 的修饰符并激活；成功返回该层数字索引，失败返回 -1。
//       实例方法注入到 GameCore::Lua::IUnit（RegisterPushMethodsTarget +
//       PushLuaMethods 标准路径），用法与 CE 版一致。
//   ModifierManager.DetachModifierByID(unitInstance, modifierID) -> boolean
//       全局表接口：移除【一层】对应的修饰符（先挂先摘）；成功返回 true。
//
//   unitInstance 是 Lua 侧的单位对象（内部经原生 Unit::GetInstance 提取）；
//   modifierID 是字符串（ModifierDefinition 行名）。
//
// 与 CE 参考实现的差异（有意修正）：
//   - CE 的 owner/result 缓冲只开 8 字节而原生写入 16/24 字节，靠堆栈侥幸存活；
//     本移植按 ABI 实际宽度开缓冲。
//   - 管理器链改走原生符号（Game::Instance::Edit -> EditGameEffects ->
//     AI::Control::Handle::get），不再绕 CE 自有 Context 全局。
//   - CE 的 Detach 当时只是硬编码索引的实验代码；本移植在 Attach 成功时记录
//     （单位, ID, 索引）三元组，Detach 按此精确摘除一层并清掉失效记录。
void ModifierManagerAPI_Init(HookAPI* api);
void ModifierManagerAPI_Shutdown();
