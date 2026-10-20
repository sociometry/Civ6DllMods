# UnitDamageCap

> 分类：**BugFix** · 返回 [工程总览](../../../README.md)

修复「单位生命值超过 100 后伤害打不死」。

## 缺陷

XP2 的 `Unit::Instance::SetDamage`（RVA `0x3A5A60`）把伤害钳到**全局常量**
`Context::Globals::ms_pkParameters->COMBAT_MAX_HIT_POINTS`（默认 100）：

```asm
13f3c5a87  44 8B 80 E8 01 00 00   mov r8d, [rax+1E8h]   ; COMBAT_MAX_HIT_POINTS
13f3c5a8e                        call Utilities::Range  ; Range(damage, 0, 上面那个)
```

Base 版（`0x18029CC40`）在同一位置多一步：按 hash `0x7D3FC711` 从
`unit->m_kProperties(+0x1F0)` 取 **`MaxHitPoints` 属性**当上限，取不到才回退全局常量。
XP2 把这段删了。

**后果**：MOD 把某单位 `MaxHitPoints` 调到 100 以上时，`SetDamage` 最多写入 100，
永远到不了 `MaxHitPoints`，单位不再死亡。

`ChangeDamage(+n)` 只是 `SetDamage(m_iDamage + n)` 的薄封装，改 `SetDamage` 一处即可
覆盖所有加伤路径。

## 修法：在函数内部装 detour

不复刻整个函数，也不「调原函数再补写」（那会写两次、事件发两次、绕过脏标记）。
做法是在**取上限那一条指令**上装 detour：

```asm
hook 点  13F3C5A87  44 8B 80 E8 01 00 00   mov r8d, [rax+1E8h]   ; 全局上限
跳回点   13F3C5A8E                            call Utilities::Range
```

detour（asmjit 生成，放在插件 DLL 的 `.uhook` 节）里调用 `GetMaxDamage(unit)` 得到真实上限，
写进 `r8d`，然后跳回 `0x13F3C5A8E`。被替换的那条 7 字节指令由 MinHook 搬进跳板，
本模块不需要调用它（我们已用自己的值替代）。

这样得到：

- `SetDamage` **只执行一次、只赋值一次**；
- `UNIT_DAMAGE_CHANGED` 只发一次，且数值就是最终值；
- `MoveAssignmentDo` 照常走，**FAutoVariable 的脏标记机制完整保留**（联机同步不受影响）；
- 除被替换的那一条指令外，函数的 Heal 取消等逻辑一个字都不动。

## InstallHook 的目标不必是函数入口

Global 的 `UnlockModelLimit` 就是这么用的 —— hook 点在 `AddResource` 函数中段
（RVA `0x940F31`），detour 末尾 `xchg+ret` 跳回中段（`jumpBack = hookAddr + 0xE`）。
本模块同理，且是这条路线在 GameCore 域的第一个用例。

**为什么用 `InstallHook` 而不是手写远跳**：MinHook 内部用 `FF 25` 绝对跳转，自己处理远距离。
本模块曾尝试手写 `jmp rel32` 补丁，实测不可行 —— 插件 DLL 与 GameCore 相距约 131000 GB，
远超 rel32 的 ±2GB（`VirtualAlloc` 拿到的地址距补丁点 1528 GB）。交给 MinHook 才是正解。

## detour 放在 `.uhook` 节：W^X 与独占一页

```cpp
#pragma section(".uhook", read, write)
__declspec(allocate(".uhook")) alignas(4096) unsigned char g_detourCode[4096];
```

- **不泄漏**：节在插件 DLL 映像内，随模块卸载一起回收。GameCore 插件每局进出都重新加载，
  用 `VirtualAlloc` 会一局泄漏一次；
- **W^X**：节声明为 read+write（不可执行），写完 `VirtualProtect(PAGE_EXECUTE_READ)`
  切成 RX 并 `FlushInstructionCache`，不留永久 RWX 页。**顺序不能反** ——
  `InstallHook` 一旦改写入口，游戏线程随时可能跳进来，那时代码页必须已可执行；
- **`alignas(4096)` 独占一页**：`VirtualProtect` 按页生效。若多个槽共用一页，
  装第二个 hook 时把页切回 RW，会让**正在执行**的第一个 detour 变成不可执行而崩溃。

**为什么 detour 必须放在 `.uhook` 节**：Loader 用 `GetModuleHandleExW(FROM_ADDRESS)` 校验
detour 地址归属，`VirtualAlloc` / `JitRuntime` 分配的页不属于任何模块，会被拒绝
（`InstallHook rejected: caller/callback is not an active plugin`）。

## asmjit 的两个坑（实测）

### 坑一：`a.jmp(addr)` 不是直接跳转

| 写法 | 实际生成 | 说明 |
| --- | --- | --- |
| `a.jmp((uint64_t)addr)` | `FF 25 02 00 00 00` + 内联指针（16 字节） | **间接跳转** —— 读目标处 8 字节当指针再跳，不是「跳到该地址」，用它必崩 |
| `a.jmp(x86::ptr(addr))` | `RelocOffsetOutOfRange` | 报错，不可用 |
| `a.jmp(x86::rax)` | `FF E0` | ✅ 先 `a.mov(x86::rax, addr)` 再跳寄存器，绝对地址、无距离限制 |

### 坑二：emit 失败会被静默丢弃

asmjit 对非法操作数只写进 emitter 的局部 `err`，`rt.add()` 仍返回 `kErrorOk`，
那条指令会被静默丢弃 —— 所以每条 emit 都必须自己检查（`UC_EMIT` / `Emit` 宏）。

## 目标

| 目标 | RVA | 说明 |
| --- | --- | --- |
| `Unit::Instance::SetDamage` | `0x3A5A60` | 补丁点 `+0x27`，回跳点 `+0x2E` |
| `Unit::Instance::GetMaxDamage` | `0x3A39A0` | 就是 XP2 里「读 MaxHitPoints 属性，取不到回退全局」那段 |

补丁前会 `memcmp` 比对原指令字节（`44 8B 80 E8 01 00 00`），不符即跳过，不会打错位置。

> `Shutdown` 是**空实现**：退局时插件整体卸载、模块内存由引擎回收，主动 `VirtualFree`
> trampoline 反而危险 —— 补丁字节还指向它。按 [插件开发指南](../../../../../../docs/PLUGIN_GUIDE.md)
> §6.3，补丁本就「按生效后不还原」设计。

## 日志

```
[UnitDamageCap] patched SetDamage clamp bound -> Unit::GetMaxDamage()
```
