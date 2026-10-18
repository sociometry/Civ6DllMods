#pragma once

struct HookAPI;

// GreatWorkAPI —— 巨作添加整合接口（Lua，APIExtension 分类）
//
// 两个"开箱即用"的 Lua 接口，无需在 Lua 侧手工组合：
//
//   GreatWorkManager.AddGreatWork(playerID, greatWorkIndex) -> bool
//       自动在玩家所有城市中找第一个可放置的（建筑+槽位），创建并放入。
//   GreatWorkManager.AddGreatWorkToCity(playerID, cityID, greatWorkIndex) -> bool
//       只在指定城市内找可放置的建筑+槽位，创建并放入。
//
// 取址走符号名（+ AOB 兜底），不再依赖裸 RVA（见 Common/GameCoreResolve.h）；
// 生命周期走 GameCore 插件的 OnLoad，不再需要 Ready/Teardown 代际回调。
void GreatWorkAPI_Init(HookAPI* api);
void GreatWorkAPI_Shutdown();
