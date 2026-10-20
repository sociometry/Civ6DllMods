#pragma once

struct HookAPI;

// GreatWorkModifierAttach —— 普通巨作创建后没有挂上 Modifier 的修复（BugFix 分类）
//
// 现象：伟人创作 / 间谍窃取 / 迁都搬移这些【普通路径】产出的巨作，
//       不会触发 Database 里挂在该 GreatWork 上的 Modifier；
//       而 Product（大商人随机产品）与 HeroEpic 产出的巨作正常。
//
// 根因（IDA 反汇编实证，见 .cpp 顶部）：挂 Modifier 的唯一入口是
//   GameEffects::GameEventHandler::OnGreatWorkAdded_AttachGameEffects，
//   它在 GameEffects::GameEventHandler 构造里订阅到
//   Game::Signals + 0x19B0 这条 ProtoSignal<void(PlayerTypes, City::Instance const*)>。
//   全库扫 ProtoSignal::Dispatch 的 75 个调用点，只有两处发 +0x19B0：
//     · GameCore::Player::Operations::Manager::Process  （移动/交换巨作）
//     · GameCore::Game::Economic::Manager::GrantRandomProduct （Product）
//   普通落点 AddGreatWork / AddGreatWorkToBuilding 一个都不发，于是 Modifier 不生效。
//
// 修法（两处）：
//   · hook Buildings::AddGreatWorkToBuilding —— 伟人创作的唯一落槽出口，
//     原函数成功后补一次 Dispatch。
//   · 导出 GreatWorkModifierAttach_NotifyAdded() —— 供本工程新增的 Lua 接口
//     在「确实新创作了一件巨作」时主动调用。
//
// 为什么不 hook Buildings::AddGreatWork：
//   它的 5 个调用者全是搬移 / 转手，没有一个是"新创作"——
//     MoveCapital / FindandMovetoNewCapital  迁都搬移
//     RelocateBuildingGreatWorks             建筑搬迁
//     ReceiveGreatWork                       收到巨作
//     Player::Cities::AddGreatWork           谍报窃取
//   在这条路径上补 Added 会让新城市重复挂载，而对应的 Removed 信号
//   （Signals+0x19C8）全库无人发射，旧城市那份永远解不掉，越搬越多。
//   所以只能由"确实新增"的调用方（本工程的 GreatWorkManager 接口）显式通知。
//
// 线程模型：OnLoad 与 detour 都在游戏主线程。
void GreatWorkModifierAttach_Init(HookAPI* api);
void GreatWorkModifierAttach_Shutdown();

// 供其它模块调用：在「确实新增了一件巨作」之后补发 GreatWorkAdded 信号。
//
// buildings  —— 接收该巨作的 City::Buildings*（内部会取 Buildings+0x00 得到 City）
// greatWorkType —— Definition::GreatWork 的索引，不是 greatWorkListIndex
//
// 为什么需要这个出口：GreatWorkManager 新增的接口走的是
// Buildings::AddGreatWork，而该函数【同时】被迁都 / 建筑搬迁 / 谍报窃取复用，
// 因此不能整体 hook 它（会让"搬移"也重复挂载）。只能由调用方在确认
// 「这是新创作、不是搬移」之后主动通知一声。
//
// 模块未加载或符号解析失败时是空实现，调用方无需判空。
void GreatWorkModifierAttach_NotifyAdded(void* buildings, int greatWorkType);