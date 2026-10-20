# UnlockModelLimit

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

3. **hook**（每个 exe 3 个）：

   - `AddResource` / `Release` 的 refcount 指令 —— 这两个函数被 IB/VB 共用，
     目标数组由运行时 `this` 决定，**单条 disp32 表达不了**；
   - `IBVB::Shutdown` 全表清理 —— 整函数替换：按 `rsi` 分流到对应扩展数组，
     并自实现 `m_DeleteList` 扩容。

4. **上限常量**：**最后**才改那 6 个立即数字节。前置重定向必须全部成功，否则整组跳过。

### fail-safe 设计

顺序不能乱：先重定向 → 再装 hook → 最后改常量。任一环节失败即**整体放弃**：
不装 hook、不打补丁、不改上限常量，游戏保持原样。

反过来（先改常量后重定向）会让句柄超出扩展数组的初始化范围，直接崩。

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
| `a.jmp((uint64_t)addr)` | `FF 25 02 00 00 00` + 内联指针（16 字节） | **间接跳转** —— 会读目标处的 8 字节当指针再跳，不是「跳到该地址」 |
| `a.jmp(x86::ptr(addr))` | `RelocOffsetOutOfRange` | 直接报错，不可用 |
| `a.jmp(Label)` | `E9 xx xx xx xx` | 能生成 `E9 rel32`，但 `Label` 只能指向同一个 `CodeHolder` 内部 |
| `a.embedUInt8(0xE9); a.embedInt32(rel)` | `E9 ...` | ✅ **可行**：asmjit 负责编码，目标地址延后回填 |

另外 **asmjit 对非法操作数只写进 emitter 的局部 `err`**，`rt.add()` 仍返回 `kErrorOk`，
那条指令会被静默丢弃 —— 所以每条 emit 都必须自己检查（见 `UnlockModelLimit_Common.h` 的
`ASMJIT_EMIT`）。

## 来源

补丁逻辑源自 **DllProxy**（`version.dll` 代理方案），后改造为 HookAPI 插件形态。

## 日志

```
UnlockModelLimit: exe image = DX12
[DX12] redirects: 58 ok, 0 failed
[DX12] installed (60 patches)
```

出现 `redirects incomplete` 说明该 exe 版本的站点形态变了，补丁会整体跳过
（功能不生效，但不改内存）。
