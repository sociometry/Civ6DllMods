#pragma once

struct HookAPI;

// InitUnitReturn —— InitUnitValidAdjacentHex 不返回单位实例的修复（BugFix 分类）
//
// 现象：Lua 侧 UnitManager.InitUnit(...) 返回 Unit 对象，
//       而 UnitManager.InitUnitValidAdjacentHex(...) 返回 nil ——
//       调用方拿到 nil，无法再对该单位做任何事。
//
// 根因（IDA 反汇编实证）：两个函数都 return 1，但只有一个真往 Lua 栈压了值。
//   lInitUnit（RVA 0x701480）尾部：
//       Dispatch(UnitPlaced);
//       ScopedInstance<IUnit,Unit::Instance>::Push(L, unit);
//       return 1;                     ← 真压了 1 个值
//   lInitUnitValidAdjacentHex（RVA 0x7015C0）尾部：
//       Placement::Set(unit, coord, 0, 0);
//       Dispatch(UnitPlaced);
//       mov eax, 1; ... retn          ← 声明返回 1 个值，栈上却什么都没有
//   Lua 按"返回 1 个值"取栈 → 取到 nil。
//
// 修法：在它返回前补一次 Push。
//   难在 detour 里拿不到 Unit::Instance* —— 该指针只活在原函数的内部寄存器里，
//   既不作为返回值传出，也没有可写入的出参。因此改为两步：
//     1) hook lInitUnitValidAdjacentHex 本身：拿到 Lua 状态 L，并标记"正处于该调用内"；
//     2) hook 它内部必走的 GameCore::Unit::Placement::Set：第一个参数就是该单位实例，
//        在标记有效时把它截下来。
//   原函数返回后，若它声称返回 1 个值却一个都没压，就用截到的实例补一次 Push。
//
// 防重复：补 Push 前比对调用前后的 Lua 栈顶。若原函数已经压过值（将来版本修好了），
//   栈顶会变，我们就不再补 —— 不会变成返回两个值。
//
// 为什么用 Placement::Set 而不是 IsValidLocation 截实例：
//   IsValidLocation 在别处也用于纯判定（不落地），Set 才是"真正放下去了"的那一步，
//   且在本函数里只调用一次。
//
// 线程模型：OnLoad 与 detour 都在游戏主线程；标记位用 thread_local 以防意外重入。
void InitUnitReturn_Init(HookAPI* api);
void InitUnitReturn_Shutdown();