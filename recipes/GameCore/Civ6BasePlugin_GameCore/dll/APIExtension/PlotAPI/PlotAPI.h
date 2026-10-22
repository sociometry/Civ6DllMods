#pragma once

struct HookAPI;

// PlotAPI —— 修改地块「魅力」的 Lua 接口（APIExtension 分类）
//
//   Plot:ChangeAppeal(n) -> boolean
//       相对增量、可累加（正增负减）。不改真实数据，增量记在插件自己的账本里，
//       读取时叠加。
//
// 【为什么不能直写真实数据】
//   魅力没有可写的字段：Plot::Instance::GetAppeal 内部走
//   Rules::Appeal::Instance::GetAppeal 现算（地形 / 地貌 / 相邻 / 建筑 …），
//   缓存字段 +0x4A 只在「GetAppeal 的 vtable+0x30 返回值 +0x1C != 0xFF」时才生效。
//   改缓存既会被下一次重算覆盖，也会污染存档。所以只做「读时叠加」。
//
// 【读拦截挂哪里】
//   挂 Plot::Instance::GetAppeal（vtable+0x28 的那个真实函数体），不是挂 6 字节跳板：
//     · 跳板（mov rax,[rcx]; jmp [rax+28h]）只拦得到"走这个桩"的调用方；
//     · 直接挂函数体，所有虚调用（含引擎内部）都经过它，覆盖率更高。
//   Lua 侧的 Plot:GetAppeal 正是经该跳板进入这个函数体（lGetAppeal 反汇编核实），
//   所以 Lua 读一定能读到叠加值。
//
// 【账本生命周期】
//   本插件是 GameCore 插件，退局 / 读档时随 GameCore 映像一起卸载，
//   OnUnload 里清空账本 —— 不存在"上一局的地块增量残留到这一局"。
//   地块索引只在同一张地图内有意义，因此换图必须清空。
//
// 线程模型：OnLoad、detour 与 Lua 回调都在游戏主线程，账本无锁。
void PlotAPI_Init(HookAPI* api);
void PlotAPI_Shutdown();
