#pragma once

struct HookAPI;

// GreatWorkPreserve —— 额外巨作槽变更导致巨作丢失修复（BugFix 分类）
//
// 缺陷：GameCore::City::Buildings::ChangeExtraGreatWorkSlots 在建筑已建成时成对调用
//   RemoveGreatWorkSlots(this, buildType);
//   AddGreatWorkSlots(this, buildType);
// RemoveGreatWorkSlots 把该建筑从 greatWorkBuildings 里删掉（槽数组一并释放），
// 而 AddGreatWorkSlots 只按 Definition::Building 的定义和 extraGreatWorkSlots 重建
// **空槽**（greatWorkIndex 一律写 -1），从不读取旧内容 —— 因此该建筑上已放置的
// 巨作全部丢失，且不会归还玩家。
//
// 触发场景：AdjustExtraGreatWorkSlots 效果改变某建筑的巨作槽数量
// （政策卡 / 奇观 / 伟人能力）。
//
// 修法：Hook RemoveGreatWorkSlots，在原函数删除之前把目标建筑的有效槽内容
// 深拷贝到插件私有暂存区；Hook AddGreatWorkSlots，在原函数返回后按 slotType
// 把内容回填到空槽上。
//
// 设计红线（勿改，详见 .cpp 顶部注释）：
//   - 绝不改变 greatWorkBuildings 数组的结构（不去重、不搬移、不改 count）。
//     MOVE_GREAT_WORK 会在 RemoveGreatWork/AddGreatWorkToSlot 之间缓存数组位置，
//     任何搬移都会让后续写入落到错误的记录上。
//   - 只写 greatWorkIndex，不改 slotType / 数组指针 / count。
//   - 只填空槽（greatWorkIndex == -1），不覆盖已有内容。
//
// 不影响 MOVE_GREAT_WORK：该操作走 RemoveGreatWork / AddGreatWorkToSlot，
// 只改单个槽的 greatWorkIndex，不经过 AddGreatWorkSlots/RemoveGreatWorkSlots。
//
// 线程模型：全部在游戏主线程，无锁。
void GreatWorkPreserve_Init(HookAPI* api);
void GreatWorkPreserve_Shutdown();
