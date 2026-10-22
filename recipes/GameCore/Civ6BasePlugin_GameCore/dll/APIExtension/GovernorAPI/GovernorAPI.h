#pragma once

struct HookAPI;

// GovernorAPI —— 设置玩家「可用总督点数」的 Lua 接口（APIExtension 分类）
//
//   Player:GetGovernors():SetAvailableGovPoints(n) -> boolean
//       设的是**当前可用点数**（不是历史总量）：已花点数保持不变，
//       总量被反推成「已花 + n」，差值交给引擎自己的 ChangeGovernorPoints。
//
// 【为什么不用「直写 +280 字段 + 手工发事件」】
//   引擎自带 GameCore::Player::Governors::ChangeGovernorPoints(int)，它内部已经处理了
//   全部收尾动作（IDA 反汇编 0x225E260 核实）：
//     · 只在 IsMajor(player) 为真时改动（城邦/蛮族天然被挡掉）
//     · 正增量时才做 clamp 与 HandleGovernorTitleMessaging（标题提示）
//     · 无论增减都发 Reporting::Event::Send(GOVERNOR_POINTS_CHANGED, playerID, delta)
//   直写字段会绕过前两条，且要自己拼事件 hash（0xDA3EF23D）—— 没有任何好处。
//
// 【为什么可用点数要靠「总量 - 已花」反推】
//   引擎只存总量（Governors+0x118 = 280），"可用" 是 UI 侧算的差值：
//       available = total - GetGovernorPointsSpent()
//   所以「设可用为 n」= 「设总量为 spent + n」，增量 = (spent + n) - total。
//
// 线程模型：OnLoad 与 Lua 回调都在游戏主线程。
void GovernorAPI_Init(HookAPI* api);
void GovernorAPI_Shutdown();
