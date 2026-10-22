# UnlockModelLimit

> 中文 | [English](README.en.md)

> 分类：**BugFix** · 返回 [工程总览](../../../README.md)

解除引擎的模型 / 材质数量上限，让大模组不至于崩。

## 问题

引擎用**静态数组**跟踪渲染资源对象，数组长度写死：

| 资源对象 | 原始上限 | 扩展后 |
| --- | --- | --- |
| IB / VB（Index / Vertex Buffer） | 8192 | 16384 |
| SPI（ShaderPackageInstance） | 16384 | 32768 |

原版资源一多就报错甚至渲染失败。模组规模一大（大量自定义模型 / 材质）必然撞上。

## 做法

**不改引擎的静态数组本身**，而是把超出原上限的句柄**改写进插件自己分配的扩展数组**。

```
句柄 < 原上限  → 走引擎原数组（完全不动）
句柄 ≥ 原上限  → 改写索引，落到插件的扩展数组
```

### 实现四步

1. **扩展数组**：`VirtualAlloc` 在 **imagebase ±2GB 内**分配（保证 disp32 可达）。
   每张表的结构是 `RefCount / records / ExtraData`，按完整 u16 句柄空间（65536 项）预留。

2. **重定向**：把引擎里读写这些数组的指令改指到扩展数组。三种形态分开处理：

   | 指令形态 | 处理方式 |
   | --- | --- |
   | `lea rcx, [rip+disp32]` | 只改 disp32 |
   | `[base+idx*4+80h]` 索引访问 | 用 asmjit **重新编码整条指令** |
   | 站点数量 | 随 exe 不同（DX12 当前 58 处） |

3. **hook**（数量随 exe 分支不同）：

   | 分支 | 数量 | 具体 |
   | --- | --- | --- |
   | DX11 | **4** | `AddResourceSPI` / `ReleaseSPI` / `AddResource` / `Release`（日志 `%d/4 hooks`）|
   | DX12 | **3** | `AddResource` / `Release` 的 refcount 指令 + `IBVB::Shutdown` 整函数替换 |

   两个分支都有的那部分：`AddResource` / `Release` 被 IB/VB 共用，目标数组由运行时 `this` 决定，
   **单条 disp32 表达不了**；DX12 另把 `IBVB::Shutdown` 整函数替换（按 `rsi` 分流到对应扩展数组，
   并自实现 `m_DeleteList` 扩容）。

4. **上限常量**：**最后**才改那 6 个立即数字节。
   ⚠️ 「前置重定向必须全部成功、否则整组跳过」**只在 DX12 分支成立**：
   DX11 的三个 `Apply*RedirectPatches` / `ApplyIBVB*Patches` 返回 `void`，单个站点失败只写一行日志，
   `ApplyLimitBytePatches()` 仍然无条件执行。DX11 的失败是**逐站点降级**，不是整组放弃。

### fail-safe 设计

顺序不能乱：先重定向 → 再装 hook → 最后改常量。

- **DX12**：任一环节失败即**整体放弃** —— 不装 hook、不打补丁、不改上限常量，游戏保持原样（日志 `redirects incomplete`）。
- **DX11**：分三步、每步内部逐站点尽力而为（失败只记日志），三步之间不互相否决。
  所以 DX11 出现部分站点失败时，功能是**部分生效**而不是整体跳过。

两个分支共同的硬约束是**顺序**：反过来（先改常量后重定向）会让句柄超出扩展数组的初始化范围，直接崩。

## 兼容性设计

- **全局对象不写死地址**：`m_IBList` / `m_VBList` / `m_SPIList` / `m_ComputeSPIList`
  用 `ResolveDataByAob` 从引用它们的 `lea` 指令里**现解 disp32**；
- **站点走 `HookInfo`**（RVA 主路径 + AOB 兜底）：RVA 落点字节不符时自动改走 AOB，
  两者都不符则放弃安装；
- **每个字节补丁都校验**原指令形态与期望立即数，不符即拒绝写入。

## 双 exe 适配

DX11 与 DX12 的 exe 是两份二进制，站点 RVA 与形态都不同：

| 分支 | 文件 |
| --- | --- |
| DX11 | `UnlockModelLimit.cpp` |
| DX12 | `UnlockModelLimit_Dx12.cpp` |
| 共享工具 | `UnlockModelLimit_Common.h`（Patch / 解码 / hook 安装 / 站点解析） |

运行期自动判别当前 exe。

## 两个 asmjit 陷阱（实测）

写 trampoline / 重编码指令时踩过：

| 写法 | 实际生成 | 说明 |
| --- | --- | --- |
| `a.jmp((uint64_t)addr)` | `FF 25 02 00 00 00` + 内联指针（16 字节） | **间接跳转**：`FF 25 rel32` 是 `jmp qword ptr [rip+rel32]`，这里的 `rel32=2` 正指向紧随其后的内联指针槽。所以它**确实**能跳到 `addr`，但那 16 字节的形态与「6 字节 `E9 rel32`」不兼容 —— 回填长度、覆写宽度全都按 6 字节算的地方会错位。**本模块已不再用它**，现在一律整条指令重编码 |
| `a.jmp(x86::ptr(addr))` | `RelocOffsetOutOfRange` | 直接报错，不可用 |
| `a.jmp(Label)` | `E9 xx xx xx xx` | 能生成 `E9 rel32`，但 `Label` 只能指向同一个 `CodeHolder` 内部 |
| `a.embedUInt8(0xE9); a.embedInt32(rel)` | `E9 ...` | ✅ 可行：asmjit 负责编码，目标地址延后回填 |

> 表中前三行是**踩坑记录**（当前代码已不含 `jmp((uint64_t))` / `FF 25` / `embedUInt8(0xE9)` 的任何用例）。
> 现在 DX12 分支的做法是对每条被改写的指令用 asmjit **整条重新编码**，不手写 `E9`。

另外 **asmjit 对非法操作数只写进 emitter 的局部 `err`**，`rt.add()` 仍返回 `kErrorOk`，
那条指令会被静默丢弃 —— 所以每条 emit 都必须自己检查（见 `UnlockModelLimit_Common.h` 的
`ASMJIT_EMIT`）。

## 来源

补丁逻辑源自 **DllProxy**（`version.dll` 代理方案），后改造为 HookAPI 插件形态。

## 日志

日志格式随分支不同（下面是格式串原文，`%zu` / `%d` 是运行时数值，不要照抄成固定数字）：

```
UnlockModelLimit: exe image = DX12
[DX12] hooks installed: %d/2
UnlockModelLimit[DX12]: installed (%zu patches)
```
```
UnlockModelLimit: exe image = DX11
UnlockModelLimit: installed (%zu patches, %d/4 hooks)
```

- DX12 若打印 `redirects incomplete`，说明该 exe 版本的站点形态变了，补丁会**整体跳过**
  （功能不生效，但不改内存）。
- DX11 没有这条整体门禁：站点失败是逐条记日志的，最终可能只生效一部分（见上文 fail-safe）。
- 两个分支都不打印固定站点数 —— 站点集合随 exe 版本变化，`.cpp` 里的 RVA 列表才是真值。
