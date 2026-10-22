#pragma once

struct HookAPI;

// GreatWorkAPI —— 巨作添加 / 移除整合接口（Lua，APIExtension 分类）
//
// 三个"开箱即用"的 Lua 接口，无需在 Lua 侧手工组合：
//
//   GreatWorkManager.AddGreatWork(playerID, greatWorkIndex) -> bool
//       自动在玩家所有城市中找第一个可放置的（建筑+槽位），创建并放入。
//   GreatWorkManager.AddGreatWorkToCity(playerID, cityID, greatWorkIndex) -> bool
//       只在指定城市内找可放置的建筑+槽位，创建并放入。
//   GreatWorkManager.RemoveGreatWork(playerID, greatWorkType) -> bool
//       从玩家所有城市里找到第一件该类型的巨作并移除（同类型有多份时只删第一件）。
//
// 注意参数口径：Add 的第二个参数叫 greatWorkIndex，Remove 的第二个参数叫
// greatWorkType —— 两者是同一个东西（Definition::GreatWork / GameInfo.GreatWorks
// 的 Index），只是按用户直觉命名。
//
// 取址走符号名（+ AOB 兜底），不再依赖裸 RVA（见 Common/GameCoreResolve.h）；
// 生命周期走 GameCore 插件的 OnLoad，不再需要 Ready/Teardown 代际回调。
void GreatWorkAPI_Init(HookAPI* api);
void GreatWorkAPI_Shutdown();
