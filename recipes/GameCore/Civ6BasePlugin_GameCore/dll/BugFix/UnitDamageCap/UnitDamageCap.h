#pragma once

struct HookAPI;

// UnitDamageCap —— 单位伤害上限对齐真实 MaxHitPoints（BugFix 分类）
//
// 缺陷（XP2 专属，Base 无此问题）：
//   GameCore::Unit::Instance::SetDamage(unit, finalDamage) 把伤害钳到全局
//   Context::Globals::ms_pkParameters->COMBAT_MAX_HIT_POINTS（默认 100）。
//   Base 版同位置先读单位自己的 MaxHitPoints 属性（hash 0x7D3FC711）作为上限，
//   读不到才回退全局值；XP2 少了这一步。
//
//   MOD 把单位 MaxHitPoints 提到 100 以上时，伤害永远到不了 MaxHitPoints，
//   单位打不死。ChangeDamage 只是 SetDamage(m_iDamage + n)，改 SetDamage 一处
//   即可覆盖所有加伤路径。
//
// ---------------------------------------------------------------------------
// 修法：只替换一条取上限的指令（不钩整个函数）
// ---------------------------------------------------------------------------
// SetDamage 里"钳制上限"只来自这一条：
//
//   13f3c5a87  44 8B 80 E8 01 00 00   mov r8d, [rax+1E8h]   ; COMBAT_MAX_HIT_POINTS
//   13f3c5a8e                        call Utilities::Range
//
// 改成 jmp 到本模块的 trampoline：那里按单位算出真实上限写进 r8d，再跳回
// 0x13f3c5a8e 继续执行。好处：
//
//   * 原函数**只执行一次、只赋值一次** —— 没有"调原函数再补写"造成的一次重复
//     写入，UNIT_DAMAGE_CHANGED 也只发一次，且数值就是最终值；
//   * MoveAssignmentDo 照常走，FAutoVariable 的脏标记机制完整保留；
//   * SetDamage 其余逻辑（Heal 取消判断等）一个字都不动。
//
// trampoline 进入时的寄存器状态（前面指令已就绪）：
//   rbx = unit          (13f3c5a7f mov rbx, rcx)
//   r9d = finalDamage   (13f3c5a76)
//   ecx = finalDamage   (13f3c5a84)
//   edx = 0             (13f3c5a82)
//   rax = ms_pkParameters（本条指令要用的，可被覆盖）
// 因此只需把上限写进 r8d，其它寄存器必须保持原值。
void UnitDamageCap_Init(HookAPI* api);
void UnitDamageCap_Shutdown();
