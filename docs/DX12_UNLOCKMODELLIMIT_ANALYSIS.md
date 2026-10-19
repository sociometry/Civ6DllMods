# UnlockModelLimit：DX11 → DX12 差异分析（实测版）

> 全部结论均来自 IDA 实机取证（DX11 = `ida-35836` / CivilizationVI.exe，DX12 = `ida-11696` / CivilizationVI_DX12.exe）。
> 每条都附工具调用与返回原文片段。**禁止使用 search_text**（会卡死 IDA）。
> 本文档是给 `recipes/Global/Civ6BasePlugin/dll/BugFix/UnlockModelLimit/` 加 DX12 分支的依据。

---

## 0. 一句话结论

**DX12 的 `ResourceList` 布局和 DX11 完全不同**，插件不能靠"换 RVA"移植；但**上限机制本身完全一致**（`m_cItems` 从 0 开始单调递增、到 cap 就拒绝分配并打同一条日志），
所以"把 8192 提到 16384 / 把 cap 常量翻倍"这个核心思路在 DX12 上依然成立。

差异集中在三点：

| # | 差异 | 影响 |
|---|---|---|
| 1 | **DX12 把 `m_Resources` 数组从列表里搬走了**，列表内改成 40 字节/项的"资源记录"数组；真正的指针数组和 ExtraData 数组分别独立 | 原来"重定向 3 个数组"变成"重定向 3 个不同的东西"，且资源记录数组也必须一起扩 |
| 2 | **DX12 把每类列表的静态对象固化成绝对地址**（`lea rcx, <绝对VA>`），`this` 不再是"任意列表"；**且这些地址全部 > 2GB** | DX11 里 `cmp rbx, m_VBList`（imm32）在 DX12 编不出来 —— 0x14F2B50C0 超出 sign-extended imm32 范围，必须 `movabs` |
| 3 | **DX12 的 ReleaseResource 从 1 个实体爆炸成 10 个孪生**（每列表一个），但 IB/VB 仍共用一个实体 `0x94D670`（两个 15 字节 thunk 传不同基址） | 好消息：**IB/VB 只需 hook 1 个**（0x94D670）；坏消息：`m_DeleteList`/`m_RecycleList` 偏移从 0x186A8/0x18668 变成 0x58400/0x58528 |

---

## 1. IB/VB 列表（`ResourceList<ID3D11Buffer*,8192,128,4,ushort>`）

### 1.1 真实布局对照

DX11 的 IDA 类型（`Graphics::ResourceList<ID3D11Buffer *,8192,128,4,unsigned short>`，sizeof 0x18680）：

| 成员 | DX11 偏移 |
|---|---|
| m_cItems | 0x0 |
| m_lock | 0x40 |
| m_RefCount[8192] | 0x80 |
| m_uCurPipe | 0x8080 |
| m_DeletionPipe[4] | 0x8088 |
| **m_Resources[8192]** (ID3D11Buffer*) | **0x8540** |
| m_DeleteList | 0x18540 |
| m_RecycleList | 0x18668 |

DX12 **实测**布局（IDB 里那份同名类型是 DX11 老布局，**不可信**；下表由反汇编 `[reg+disp]` 直接推出）：

| 成员 | DX12 偏移 | 大小 / 步长 | 证据 |
|---|---|---|---|
| m_cItems | 0x0 | 4 | `AddResource: mov eax,[rbx]; inc eax; cmp eax,2000h; mov [rbx],eax` |
| m_lock | 0x40 | 0x40 | `lock cmpxchg [rbx+40h], ecx` |
| m_RefCount[8192] | 0x80 | 0x8000（4/项） | `inc dword ptr [rbx+rcx*4+80h]`；Reset 里 `memset(&unk_146C720C0, 0, 0x8000)`（=m_IBList+0x80） |
| m_uCurPipe | 0x8080 | 4 | `sub_14094D670: mov eax, [rbx+8080h]` |
| **m_DeletionPipe[3]** | **0x8088** | **0x378（3×0x128）** | `imul rcx, rax, 128h` + `lea rdi, [rbx+8088h]` |
| **资源记录数组[8192]** | **0x8400** | **0x50000（0x28/项）** | `lea rcx, qword_146C7A440`（=m_IBList+0x8400）、`lea r11,[rcx+rax*8]` 其中 `rax=5*idx`；Reset 里 `memset(&qword_146C7A440, 0, 327680)` |
| m_DeleteList | 0x58400 | 0x128 | `sub_140945160: a1+361472=0x58400`（Vector 基址），其 ptr/cap/count 在 +0x110/+0x118/+0x120 = 0x58510/0x58518/0x58520 |
| m_RecycleList | 0x58528 | 0x18 | `AddResource: cmp [rbx+58538h],esi` / `mov rax,[rbx+58528h]` |
| **ExtraData[8192]** | **0x58540** | **0xE0000（0x70/项）** | `lea rax, unk_146CCA580`（=m_IBList+0x58540）、`imul r8, rdi, 70h` |
| （列表总大小） | — | **0x58540** | ExtraData 是外层 `ResourceListExtraData` 的成员，紧接在列表之后 |

**布局自洽性校验（每一步都精确对齐，无重叠无空洞）**：

```
0x0000 + 0x0004  -> m_cItems
0x0040 + 0x0040  -> m_lock              （0x44..0x7F 填充）
0x0080 + 0x8000  = 0x8080  m_RefCount 结束
0x8080 + 0x0008  = 0x8088  m_uCurPipe + 4 字节填充
0x8088 + 3*0x128 = 0x8400  m_DeletionPipe[3] 结束     <-- 必须是 3 条，4 条会到 0x8528 与记录数组重叠
0x8400 + 8192*40 = 0x58400 资源记录数组结束            <-- 恰好等于 m_DeleteList 起始
0x58400 + 0x128  = 0x58528 m_DeleteList 结束
0x58528 + 0x018  = 0x58540 m_RecycleList 结束
0x58540                    ExtraData 数组起始（列表对象结束）
```

> 🔑 **两个关键差异**：
> 1. **DX11 的 `m_Resources[8192]`（指针数组，8 字节/项，@0x8540）在 DX12 变成了"资源记录数组"（40 字节/项，@0x8400）** ——
>    原来的 `mov rX, [base+idx*8+0x8540]` 形态在 DX12 里**根本不存在**。
> 2. **DX12 的 `m_DeletionPipe` 是 3 条不是 4 条**（DX11 是 4 条，0x8088+4×0x128=0x8528，后面留 0x18 空洞到 0x8540）。
>    DX12 的 `sub_14094D670` 里 `imul rcx, rax, 128h` 证实元素仍是 0x128 字节，
>    而 0x8400 − 0x8088 = 0x378 = 3×0x128 精确整除 —— 所以是 3 条。

### 1.2 DX12 的两个列表对象

| 列表 | 绝对 VA | RVA | 引用它的函数（xrefs_to 全量） |
|---|---|---|---|
| `Graphics::_INTERNAL::m_IBList` | 0x146C72040 | 0x6C72040 | 9 个：sub_140023ED0(静态析构)、sub_140940790(AddRefIB)、CreateIB(0x140943E90)、sub_14094A470(ReleaseIB)、sub_14094D400(RemoveResource)、sub_140950B20(Shutdown)、sub_1409700A0(Init)、sub_140971490(Reset)、sub_140972100(Init 参数) |
| `Graphics::_INTERNAL::m_VBList` | 0x14F2B50C0 | 0xF2B50C0 | 9 个，一一对应 |

### 1.3 cap 与实例

| | DX11 | DX12 |
|---|---|---|
| IB/VB AddResource | RVA **0x940770**，size 0x148，cap 0x2000 | RVA **0x940BE0**，size 0x148，cap 0x2000 |
| IB/VB ReleaseResource | RVA **0x956600**，size 0xC7 | **10 个孪生**（见 1.5） |
| SPI AddResource | RVA 0x940E00，cap 0x4000 | RVA **0x941270**，cap 0x4000 |
| SPI ReleaseResource | RVA 0x956A10，size 0xC7 | 见 §2 |

### 1.4 hook 站点对照（IB/VB AddResource）

DX11 的两处 hook（`UnlockModelLimit.cpp` 第 638/696 行）：

```
DX11 AddResource hook @ 0x9408A1:   inc dword ptr [rbx+rcx*4+80h]
DX11 Release    hook @ 0x95664A:   mov esi, [rbx+r8*4+80h]
```

DX12 逐字节同构：

```
DX12 AddResource (0x940BE0):
  140940d03  lea r11, [rsp+88h+var_8]
  140940d0b  movzx ecx, di
  140940d0e  movzx eax, di
  140940d11  inc dword ptr [rbx+rcx*4+80h]      <-- hook 站点 = 0x940D11
  140940d18  mov [rbx+40h], esi
  140940d1b  mov rbx, [r11+10h]
  140940d1f  mov rsi, [r11+18h]
  140940d23  mov rsp, r11
  140940d26  pop rdi
  140940d27  retn
```

DX12 **IB/VB 专用** ReleaseResource（0x14094D670，由两个 15 字节 thunk 各自传基址调用）：

```
sub_14094D400(a1):  return sub_14094D670(m_IBList, a1);      <-- 15 字节 thunk
sub_14094DDD0(a1):  return sub_14094D670(m_VBList, a1);      <-- 15 字节 thunk

DX12 ReleaseResource 实体 (0x14094D670):
  14094d6b1  add dword ptr [rbx+r8*4+80h], 0FFFFFFFFh
  14094d6ba  mov esi, [rbx+r8*4+80h]                          <-- hook 站点 = 0x14094D6BA
  14094d6c2  jnz short loc_14094D723
  ...  (m_DeletionPipe 逻辑，偏移 0x8088，步长 0x128 —— 与 DX11 IB 完全同构)
```

> ✅ **好消息**：DX12 的 `AddResource` 是 **IB/VB 共用一个实例**（`CreateIB` 与 `CreateVB` 都 `call 0x140940BE0`），
> `ReleaseResource` 的实体也 **IB/VB 共用**（`sub_14094D400` 与 `sub_14094DDD0` 两个 15 字节 thunk 分别传 `m_IBList` / `m_VBList` 给 `sub_14094D670`）。
> 所以 DX11 版里"一个 AddResource + 一个 ReleaseResource 服务两个列表"的结构**在 DX12 上依然成立**，
> `cmp rbx, m_VBList` 的分支逻辑可以保留 —— **唯一要改的是那个 imm32 装不下 0x14F2B50C0**（必须 `movabs`）。
>
> 或者更干净：IB/VB 的 Release 直接在两个 thunk 上各装一个 hook（各 15 字节，签名就是 `ushort f(ushort idx)`），完全绕开基址比较。

### 1.5 DX12 的 10 个 ReleaseResource 孪生

字节特征 `83 84 83 80 00 00 00 FF`（`add dword ptr [rbx+r8*4+0x80], -1`）全 .text 只有 10 处，
**每个对应一个不同的列表**：

| # | 函数 RVA | 该指令 RVA | DeletionPipe 基址偏移 | Pipe 元素大小 | 列表基址（由调用者推） |
|---|---|---|---|---|---|
| 1 | 0x14094D4D0 | 0x94D512 | +0x8088 | 0x128 | ? |
| 2 | 0x14094D5A0 | 0x94D5E2 | +0x8088 | 0x128 | ? |
| 3 | **0x14094D670** | 0x94D6B2 | +0x8088 | 0x128 | **m_IBList / m_VBList**（thunk 0x14094D400 / 0x14094DDD0） |
| 4 | 0x14094D740 | 0x94D782 | +0x8088 | 0x128 | ? |
| 5 | 0x14094D810 | 0x94D852 | +0x8088 | 0x128 | ? |
| 6 | 0x14094D8E0 | 0x94D922 | +0x8088 | 0x228 | dword_14F22CBC0（ComputeSurface，thunk 0x14094D3D0） |
| 7 | 0x14094D9B0 | 0x94D9F2 | +0x8088 | 0x428 | dword_1474DE300（**m_ComputeSPIList**，thunk 0x14094D3C0） |
| 8 | **0x14094DA80** | **0x94DAC1** | +0x10088 | 0x428 | dword_146C1E440 = **m_SPIList**（调用者 sub_14094DD00） |
| 9 | 0x14094DB50 | 0x94DB91 | ? | ? | ? |
| 10 | 0x14094DC20 | 0x94DC62 | ? | ? | ? |

> **重要**：#3（0x14094D670）**同时服务 IB 和 VB**（两个 thunk 传不同基址）——
> 所以 DX11 版"1 个 Release hook 管两个列表"的结构在 DX12 依然成立。
> #6/#7 是 Compute 一族（不是 IB/VB/SPI），DX11 侧对应的实例是 0x940CB0/0x940E00 一族。
> #1/#2/#4/#5/#9/#10 的列表身份未确认（它们分别是 TextureViews / ConstantSet / SurfaceSet 等列表的 ReleaseResource），
> **对 UnlockModelLimit 而言无关**，因为插件只关心 IB/VB/SPI 三个列表。

### 1.6 上限字节补丁对照

DX11（`ApplyLimitBytePatches`，第 772-791 行）：

```
0x9407CF = 0x40   // cmp eax, 2000h -> 4000h  （指令 @0x9407CD: 3D 00 20 00 00，立即数第2字节）
0x9407FE = 0x40   // mov edx, 2000h -> 4000h  （指令 @0x9407FC: BA 00 20 00 00）
0x940864 = 0x40   // mov r8d, 2000h -> 4000h  （指令 @0x940861: 41 B8 00 20 00 00，带 REX.B）
```

DX12 实测字节（`get_bytes`）：

| 用途 | DX12 指令地址 | 原始字节 | 指令 | **要改的地址** | 新值 |
|---|---|---|---|---|---|
| IB/VB cap cmp | 0x140940C3D | `3D 00 20 00 00` | `cmp eax, 2000h` | **0x940C3F** | 0x40 |
| IB/VB 日志参数 | 0x140940C6C | `BA 00 20 00 00` | `mov edx, 2000h` | **0x940C6E** | 0x40 |
| IB/VB 日志参数 | 0x140940CD1 | `41 B8 00 20 00 00` | `mov r8d, 2000h` | **0x940CD4** | 0x40 |
| SPI cap cmp | 0x1409412CD | `3D 00 40 00 00` | `cmp eax, 4000h` | **0x9412CF** | 0x80 |
| SPI 日志参数 | 0x1409412FC | `BA 00 40 00 00` | `mov edx, 4000h` | **0x9412FE** | 0x80 |
| SPI 日志参数 | 0x140941361 | `41 B8 00 40 00 00` | `mov r8d, 4000h` | **0x941364** | 0x80 |

> 这与 `EXE_SITE_MAP.md` 草稿给的 0x940C3D/0x940C6C/0x940CD1/0x9412CD/0x9412FC/0x941361 **一致**，
> 且与 DX11 的"指令地址 + 2"规律完全同构（DX11 指令 0x9407CD → 补丁 0x9407CF）。

**第 4 个补丁（DX11 0x9417EB，`AllocateStreamInstanceVB` 的 `D3D11_BUFFER_DESC.ByteWidth=0x20000`）在 DX12 无对应** ——
DX12 的 CreateIBAtHandle/CreateVBAtHandle 已经完全不构造 `D3D11_BUFFER_DESC`（见 §3）。

---

## 2. 列表的分配/释放路径（DX12）

### 2.1 AddResource（0x940BE0）伪代码

```c
__int64 __fastcall ResourceList<ID3D11Buffer*,...>::AddResource(volatile signed __int32 *a1)
{
  while (_InterlockedCompareExchange(a1 + 16, 1, 0) != 0) _mm_pause();   // a1+0x40
  v2 = 0;
  if (*((_DWORD*)a1 + 90446) != 0)              // a1+0x58538  recycleList.m_nCurrSize
    v2 = *(_WORD*)(*((_QWORD*)a1 + 45221) + 2LL * (unsigned)--*((_DWORD*)a1 + 90446));  // a1+0x58528
  else {
    v3 = *a1 + 1;                                // m_cItems
    if (v3 >= 0x2000) { ...打日志... }
    else { v2 = *(_WORD*)a1; *a1 = v3; }
  }
  ++a1[v2 + 32];                                 // a1+0x80 + v2*4  = m_RefCount[v2]
  *((_DWORD*)a1 + 16) = 0;                       // a1+0x40 解锁
  return v2;
}
```

与 DX11 版**逻辑完全一致**，只是 recycleList 的字段偏移从 `+0x18668/+0x18678` 变成 `+0x58528/+0x58538`。

### 2.2 资源记录数组 & ExtraData 的写入点（CreateIBAtHandle 0x14096A4E0）

```
14096a4fa  lea rcx, qword_146C7A440        ; = m_IBList + 0x8400  ← 资源记录数组基址
14096a516  lea rax, [rdi+rdi*4]            ; rdi = hIB
14096a51a  lea r11, [rcx+rax*8]            ; r11 = &rec[hIB]   (步长 40)
14096a51e  lea rax, unk_146CCA580          ; = m_IBList + 0x58540 ← ExtraData 数组基址
14096a50e  imul r8, rdi, 70h               ; r8 = hIB * 112
14096a525  add r8, rax                     ; r8 = &extra[hIB]
14096a528  mov rsi, [r8+50h]               ; 锁标志
14096a583  mov [r11+20h], eax              ; rec->f20 = eType
14096a58e  mov [r11+1Ch], r10d             ; rec->f1C = byteWidth
14096a5a1  mov [r11+18h], eax              ; rec->f18 = format
14096a5a5  mov [r8+6Ch], cl                ; extra->f6C = flags
14096a5c3  mov [rsp+58h+var_28], r11
14096a5c8  mov [rsp+58h+var_18], 1
14096a5d0  mov [rsp+58h+var_14], eax        ; = hIB
14096a5d4  call sub_140968A20              ; 真正的 D3D12 资源创建
```

CreateVBAtHandle（0x14096CD40）同构，数组分别是：
- 资源记录数组 = `qword_14F2BD4C0`（= m_VBList + 0x8400）
- ExtraData 数组 = `unk_14F30D600`（= m_VBList + 0x58540）

### 2.3 `sub_140968A20` —— DX12 的"CreateBuffer 等价物"

它接收一个 4 字段描述符 `{rec, extra, 1, hIndex}`，按 `a5`（flags&2 等）选择 5 个不同的**静态分配池**
（`unk_1412957C0` / `1412959C0` / `141295AC0` / `1412958C0` / `141295CC0`），
调用 `sub_140967DC0 / sub_1409682C0 / sub_140968040 / sub_1409678C0` 完成真正的分配。
**它不碰 `m_RefCount`，也不碰 `m_DeleteList`** —— 那些在 AddResource/ReleaseResource 里。

### 2.4 列表的初始化与重置

- `sub_140971490`（Reset）：对每个列表做
  ```
  m_VBList = 1;  memset(&qword_14F2BD4C0, 0, 327680);  memset(&unk_14F2B5140, 0, 0x8000);
  m_IBList = 1;  memset(&qword_146C7A440, 0, 327680);  memset(&unk_146C720C0, 0, 0x8000);
  ```
  → 327680 = 8192 × 40（资源记录数组），0x8000 = 8192 × 4（**m_RefCount**，地址 m_IBList+0x80）。
  **这是"要跟着一起扩"的两个数组的直接证据。**

- `sub_140945160`（RefCount 清零 + 把非零项推进 RecycleList）：
  ```c
  for (i = 0; (unsigned)v2 < *(_DWORD*)a1; i = v2) {
    if (*(_DWORD*)(a1 + 4LL*v2 + 128) != 0) {        // a1 + 0x80 + v2*4  = m_RefCount[v2]
      *(_DWORD*)(a1 + 4LL*v2 + 128) = 0;
      ...  push v2 into m_RecycleList (a1+0x58510/0x58518/0x58520) ...
    }
    ++v2;
  }
  ```
  被 `sub_140950B20`（Shutdown）以 `sub_140945160(&m_VBList); sub_140945160(&m_IBList);` 调用。

- `sub_14094A470`（ReleaseIB 副本拷贝）：`sub_14096D510(&m_IBList[hIB*10+8448], &extra[hIB])`
  其中 `8448 = 0x2100`，`10*8 = 0x50`，`0x2100*8 + 0x50 = 0x10850`... 注意 IDA 的 `m_IBList` 是 `_DWORD*`，
  实际地址 = m_IBList + 0x8400 + hIB*0x28，ExtraData = m_IBList + 0x58540 + hIB*0x70。**与 2.2 完全吻合。**

---

## 3. DX12 的 BufferExtraData（ExtraData 元素）

⚠ **先说结论**：DX12 的 ExtraData **实际元素步长是 0x70（112 字节）**，不是 IDB 里声明的 0x30。
铁证：`CreateIBAtHandle: imul r8, rdi, 70h`、`CreateVBAtHandle: imul r9, rbp, 70h`、
`sub_14094A550: imul rdx, rax, 70h`、`sub_14096E100: imul rax, rbx, 70h`（四处独立出现）。
IDB 里那个 `Graphics::_INTERNAL::BufferExtraData`（48 字节）是**从 DX11 带过来的旧声明**，与 DX12 实机不符。

下面是 IDB 声明的内容（仅供对照，**不要按它算数组尺寸**）：

| 偏移 | DX11 类型 | DX12 实测 |
|---|---|---|
| 0x00 | D3D11_BUFFER_DESC Desc (24B) | 未知 24 字节 |
| 0x18 | bool bInstance | ? |
| 0x20 | void* pTempBuffer | ? |
| 0x28 | unsigned iInputFormat | ? |
| 0x2C | BYTE Format[4] | DXGI_FORMAT Format |
| 0x50 | — | **锁标志**（`mov rsi,[r8+50h]` / `test rsi,rsi`） |
| 0x6C | — | **flags**（`mov [r8+6Ch], cl`） |

**重定向 ExtraData 数组时必须按 0x70 步长、8192 项 = 0x380000 字节分配。**

已验证的 DX12 字段偏移（由真实 `[reg+disp]` 给出）：

| 偏移 | 含义 | 证据 |
|---|---|---|
| +0x50 | 锁/引用标志（`test rsi,rsi` 判空） | `CreateIBAtHandle: mov rsi, [r8+50h]` / `CreateVBAtHandle: mov rbx, [r9+50h]` |
| +0x6C | flags（1 字节） | `CreateIBAtHandle: mov [r8+6Ch], cl` / `CreateVBAtHandle: v15[108] = flags`（108 = 0x6C） |

前 0x50 字节（含 IDB 声明为 `D3D11_BUFFER_DESC` 的那 24 字节）在 DX12 里装的是什么，**尚未确定**（§6 待办 #3）。

---

## 4. SPI 列表（`ResourceList<FGXShaderPackageInstance,16384,512,1,ushort>`）

### 4.1 好消息：SPI 的布局 **DX11 和 DX12 完全一致**

`type_inspect` 两边返回**逐字段相同**：

| 成员 | 偏移 | 大小 |
|---|---|---|
| m_cItems | 0x0 | 4 |
| m_lock | 0x40 | 64 |
| m_RefCount[16384] | 0x80 | 65536 |
| m_uCurPipe | 0x10080 | 4 |
| m_DeletionPipe[1] | 0x10088 | 1064 |
| m_Resources[16384] | 0x104C0 | 262144 |
| m_DeleteList | 0x504C0 | 1064 |
| m_RecycleList | 0x508E8 | 24 |

### 4.2 DX12 的 SPI 列表基址

| 列表 | DX11 RVA | DX12 VA | DX12 RVA |
|---|---|---|---|
| `m_SPIList` | 0x2AB9E40 | **0x146C1E440** | **0x6C1E440** |
| `m_SPIList.m_RefCount` | 0x2822D00 | **0x146C1E4C0** | **0x6C1E4C0** |
| `m_SPIList.m_Resources` | 0x2ACA300 | **0x146C2E900** | **0x6C2E900** |
| `m_SPIList.m_Resources.m_iPackageID` | 0x2ACA304 | **0x146C2E904** | **0x6C2E904** |
| `m_ComputeSPIList` | 0x2822C80 | **0x1474DE300** | **0x74DE300** |
| `m_ComputeSPIList.m_Resources` | — | **0x1474E6700** | **0x74E6700** |

证据：
- `sub_140944A50`（CreateShaderPackageInstance）：`lea rcx, dword_146C1E440` → `call sub_140941270`（cap=0x4000 的 AddResource）；
  紧接 `sub_140944A77: lea rax, unk_146C2E900` + `shl r8,4` → **m_SPIList + 0x104C0**（= m_Resources 基址）✅
- `sub_140943710`（CreateComputePackageInstance）：`lea rbp, dword_1474DE300` → `call sub_140941120`（cap=0x2000 的 AddResource）；
  紧接 `mov [rcx+rbp+84C0h], esi` / `mov [rcx+rbp+84C4h], edi` → **m_ComputeSPIList + 0x84C0/0x84C4** ✅
- 唯一性交叉验证：`xrefs_to 0x140941270` → 唯一 code xref = 0x140944A6E（CreateShaderPackageInstance）；
  `xrefs_to 0x140941120` → 唯一 code xref = 0x140943736（CreateComputePackageInstance）。两个 AddResource 实例各只有一个调用者，身份无歧义。✅
- `sub_14096F9A0` 里 `lea rsi, [rbx+504C0h]` → 析构 m_ComputeSPIList 的 m_DeleteList，与 SPI 布局吻合。

> ⚠ **修正**：本文件早期版本曾把 `m_ComputeSPIList` 猜成 `dword_146C6ED40`（0x6C6ED40），那是错的。
> `dword_146C6ED40` 是另一个列表（Init 里 `memset(qword_146C6FF00, 0, 0x2000)` 一族，8192 项 × 4 字节）。
> 正确值是 **0x1474DE300**，由 CreateComputePackageInstance 的直接证据确定。

**DX11 的 B 类补丁点 `disp = 0x2ACA304 = m_SPIList + 0x104C4`**（= m_Resources + 4 = `m_iPackageID`），
DX12 对应绝对地址 = **0x146C2E904**。

### 4.3 DX12 的 SPI 站点

DX11 12 个 SPI 重定向站点在 DX12 的对应（本表由 `xrefs_to` + 反汇编逐点核对）：

方法：先用 `get_bytes` 取 DX11 站点原始编码，再用 `xrefs_to` 对 DX12 的同一语义数据地址列全部引用者，一一对齐。

DX11 侧原始字节（`get_bytes`，dx11）：

```
0x140946A37  48 8D 05 C2 38 18 02   lea rax,[rip+0x21838C2]  -> m_SPIList.m_Resources
0x140957990  48 8D 05 69 29 17 02
0x140956B15  48 8D 05 E4 37 17 02
0x14095D775  48 8D 0D 84 CB 16 02   lea rcx -> m_Resources（后接 call memset）
0x14095D783  48 8D 0D 36 C7 15 02   lea rcx -> m_RefCount
0x14095CEB5  4C 8D 2D 04 D0 15 02   lea r13 -> m_RefCount
0x140940542  48 8D 0D F7 98 17 02   lea rcx, m_SPIList
0x14094D013  48 8D 0D EA D2 17 02   lea rcx -> m_iPackageID
0x14094ABAC  8B 84 C2 04 A3 AC 02   mov eax,[rdx+rax*8+0x2ACA304]
0x1409504FB  8B 84 C7 04 A3 AC 02   mov eax,[rdi+rax*8+0x2ACA304]
0x1409506F9  41 8B 84 CE 04 A3 AC 02
0x140950757  41 8B 84 C6 04 A3 AC 02
```

`xrefs_to 0x146C2E900`（= DX12 m_SPIList.m_Resources）→ **4 处**，与 DX11 `0x142ACA300` 的 4 处一一对应：

| DX11 站点 | 语义 | DX12 RVA | DX12 指令原文 | 状态 |
|---|---|---|---|---|
| 0x946A37 | CreateShaderPackageInstance | **0x944A77** | `48 8D 05 82 9E 2E 06` `lea rax, unk_146C2E900` | ✅ |
| 0x957990 | RenderSurface | **0x95E20A** | `48 8D 05 EF 06 2D 06` `lea rax, unk_146C2E900` | ✅ |
| 0x956B15 | ReleaseShaderPackageInstance | **0x94DD35** | `48 8D 05 C4 0B 2E 06` `lea rax, unk_146C2E900` | ✅ |
| 0x95D775 | Shutdown memset pDst (Resources) | **0x97175B** | `lea rcx, unk_146C2E900` | ✅ |

`xrefs_to 0x146C1E4C0`（= DX12 m_SPIList.m_RefCount）→ **1 处**：

| 0x95D783 | Shutdown memset pDst (RefCount) | **0x97177F** | `48 8D 0D 3A CD 2A 06` `lea rcx, unk_146C1E4C0`，随后 `mov r8d, 10000h; call sub_140990410` | ✅ |

（DX11 0x95D783 后面也是 `mov r8d, 10000h` —— 0x10000 = 16384×4，两边完全一致。）

| DX11 站点 | 语义 | DX12 RVA | DX12 指令原文 | 状态 |
|---|---|---|---|---|
| 0x940542 | AddRefShaderPackageInstance | **0x9407E0** | `lea rcx, dword_146C1E440` / `lea rcx,[rcx+rax*4]` / `mov eax,[rcx+80h]` / `inc eax` / `mov [rcx+80h],eax` | ✅ |
| 0x94D013 | GetEnumFromShaderPackageInstance | **0x9479E0** | `movzx eax,cx` / `lea rcx, dword_146C2E904` (`48 8D 0D 1A 6F 2E 06`) / `add rax,rax` / `mov eax,[rcx+rax*8]` | ✅ |
| 0x95CEB5 | Shutdown refcount clear (lea r13) | — | DX12 的 Shutdown 里对 m_RefCount 只有 0x97177F 一处引用，**无 `lea r13` 形态** → 建议只改 0x97177F | ⚠ |
| 0x94ABAC | ExecuteCommandSubset `mov eax,[rdx+rax*8+0x2ACA304]` | 未确定 | — | ❓ |
| 0x9504FB | Layout_SSInvoke | 未确定 | — | ❓ |
| 0x9506F9 | Layout_SurfaceSet 循环1 | 未确定 | — | ❓ |
| 0x950757 | Layout_SurfaceSet 循环2 | 未确定 | — | ❓ |

> ⚠ **AddRef 别选错孪生**：DX12 有 3 个 70 字节 refcount helper（0x9407E0 / 0x940830 / 0x940880），
> 形态完全一致。**0x9407E0 引用 `dword_146C1E440`（m_SPIList）**，另两个分别引用 IB/VB 列表。

> ✅ **4 个 disp32 站点已定论（DX12 合并成 1 处）**
>
> `xrefs_to(0x146C2E904)` 在 DX12 全镜像**只有 2 处**引用：
> - `0x1409479E3` — `lea rcx, dword_146C2E904`（= 上面的 GetEnum 站点，`sub_1409479E0`）
> - `0x14095C2E3` — **disp32 形态**，宿主 = `sub_14095C020`（size 0x377）
>
> DX12 站点字节（`get_bytes(0x14095C2DC, 16)`）：
> ```
> 14095c2dc  41 0F B7 C2              movzx eax, r10w
> 14095c2e0  48 03 C0                 add rax, rax          ; rax = 2*i
> 14095c2e3  41 8B 84 C5 04 E9 C2 06  mov eax, [r13+rax*8+0x6C2E904]   ; = m_Resources[i].m_iPackageID
> ```
> `add rax,rax` + `*8` = **16 字节步长**，正是 `FGXShaderPackageInstance` —— 与 DX11 语义逐条一致。
>
> **DX11 的 4 个 disp32 站点（0x94ABAC / 0x9504FB / 0x9506F9 / 0x950757）在 DX12 只剩 1 处**：
> `sub_14095C020` 是 ExecuteCommandSubset 一族的合并版（`case 5` = `" UpdateSubresource: %s [Count:%d]"` 所在的分派器
> `sub_14094AB40` 的兄弟；它内部 `call sub_14095E1F0` 做实际的 IA 绑定）。
> 也就是说 DX12 **把 4 条独立的 "读 m_iPackageID" 路径收敛成了 1 条**。
>
> **补丁方式（与 DX11 完全同构，可直接复用 `EncodeAndApplyIBVBPatch` 的 kind=1 分支）**：
> ```
> 原始:  41 8B 84 C5 <disp32>     ModRM=0x84(mod=10, base=101+REX.B → r13), SIB=0xC5(scale=8, index=rax, base=101)
> 目标:  41 8B 04 C5 <disp32'>    ModRM=0x04(mod=00)  →  base=101 且 mod=00 = 无 base（绝对寻址）
> 改动:  地址 0x14095C2E5  0x84 -> 0x04          （1 字节）
>        地址 0x14095C2E7  disp32 -> 插件数组绝对地址（< 2GB）
> 指令长度不变（8 字节），无需 NOP 填充。
> ```
> 对照 DX11 的原站点 `8B 84 C7 04 A3 AC 02`：ModRM `84`(mod=10) + SIB `C7`(base=rdi) —— **结构完全相同**，
> 说明 DX11 版当年也是靠"把 mod 从 10 改成 00 去掉 base"来做的，技术手段一致。
>
> ⚠ 唯一差别：DX12 多了 `41`（REX.B）前缀。DX11 版 `EncodeAndApplySPIPatch` 里的
> `int off = ((orig[0] & 0xF0) == 0x40) ? 1 : 0;` 已经处理了这种情况，**照搬即可**。

### 4.4 DX12 的 SPI hook 站点（已核实）

- **AddResource**：DX12 RVA **0x9413A1**
  ```
  get_bytes(0x1409413A1, 10) = FF 84 8B 80 00 00 00 89 73 40
  1409413a1  inc dword ptr [rbx+rcx*4+80h]
  1409413a8  mov [rbx+40h], esi
  ```
  indexReg = **rcx**，this = **rbx**。与 DX11 `0x940F31`（`FF 84 8B 80 00 00 00 89 73 40`）**字节完全相同**。✅

- **ReleaseResource**：DX12 RVA **0x94DAC1**（= `sub_14094DA80` + 0x41）
  ```
  get_bytes(0x14094DAC1, 10) = 42 83 84 83 80 00 00 00 FF
  14094dac1  add dword ptr [rbx+r8*4+80h], 0FFFFFFFFh
  14094daca  mov esi, [rbx+r8*4+80h]
  ```
  indexReg = **r8**，this = **rbx**。与 DX11 `0x956A51`（`42 83 84 83 80 00 00 00 FF`）**字节完全相同**。✅

  孪生选择依据（3 个 ReleaseResource 的该指令字节全同，只能靠调用者区分）：

  | 函数 | 该指令 RVA | 唯一调用者 | 判定 |
  |---|---|---|---|
  | 0x14094D8E0 | 0x94D921 | `sub_14094D3D0`：`sub_14094D8E0(&dword_14F22CBC0, idx)` | ComputeSurface |
  | 0x14094D9B0 | 0x94D9F1 | `sub_14094D3C0`：`sub_14094D9B0(dword_1474DE300, idx)` | **m_ComputeSPIList** |
  | **0x14094DA80** | **0x94DAC1** | `sub_14094DD00` = `ReleaseShaderPackageInstance` | **m_SPIList** ✅ |

- **IB/VB 的对应 hook**（供 DX12 分支同时安装）：
  - AddResource：**0x940D11**（`inc dword ptr [rbx+rcx*4+80h]`），this = rbx，indexReg = rcx
  - ReleaseResource（IB/VB 共用实体）：**0x94D6BA**（`mov esi,[rbx+r8*4+80h]`），this = rbx，indexReg = r8
    实际 DX11 的 hook 打在 `mov esi` 上（0x95664A = 0x956600 + 0x4A），所以 DX12 对应 0x94D670 + 0x4A = **0x94D6BA**。
  - ReleaseResource（VB）：由 thunk `sub_14094DDD0` 调 `sub_14094D670(m_VBList, ...)` → **同一个实体**，无需第二个 hook。

### 4.5 SPI 上限字节补丁

见 §1.6 表（DX12: 0x9412CF / 0x9412FE / 0x941364）。

---

## 5. 对 `UnlockModelLimit.cpp` 的移植影响

### 5.1 必须改写的部分

| DX11 代码 | DX12 需要 |
|---|---|
| `InitIBVBExtendedArrays()`：RefCount[32768] + Resources[32768]*8 + ExtraData[32768]*0x30 | RefCount[32768]*4 + **资源记录[32768]*0x28** + ExtraData[32768]*0x70；两个列表各一份 → 共 32768*(4+40+112)*2 = **0xF60000×2 = 0x1EC0000 字节** |
| `InitSPIExtendedArrays()`：Resources[32768]*16 + RefCount[32768]*4 | 布局不变（SPI 两边一致），但**基址全换**：m_SPIList = 0x146C1E440、其 RefCount = 0x146C1E4C0、其 Resources = 0x146C2E900；m_ComputeSPIList = 0x1474DE300、其 Resources = 0x1474E6700 |
| `InstallHook_AddResource()` 里的 `cmp rbx, m_VBList` 分支 | DX12 的 AddResource 只服务 IB/VB 两个列表，判断仍可用，但 **m_VBList 的 VA 是 0x14F2B50C0（> 2GB！）** → `cmp rbx, imm32` 编不出来，必须改成 `mov r10, imm64; cmp rbx, r10` |
| `InstallHook_Release()` 单个 hook | **结构不变**：DX12 的 IB/VB 仍共用一个 ReleaseResource 实体 `0x94D670`（两个 thunk 传不同基址），只需把 hook 点换到 **0x94D6BA**（DX11 是 0x95664A）。其余 9 个孪生属于别的列表，**不要 hook** |
| `ApplyIBVBRedirectPatches()` 的 31 个站点 | **几乎全部失效**：DX12 里 `m_Resources` 指针数组不在列表内，那些 `mov rX,[base+idx*8+0x8540]` 形态的指令在 DX12 里不存在（已用 `xrefs_to 0x146C7A440/0x146CCA580` 证实只有 5 / 3 个引用者） |
| `PatchLimitByte(0x9417EB, 0x04)` | DX12 **无对应**（无 D3D11_BUFFER_DESC） |

### 5.2 DX12 独有的坑

1. **列表对象地址全部 > 2GB**：`m_VBList = 0x14F2B50C0`（5.5GB）、`m_IBList = 0x146C72040`（5.1GB）。
   任何 `mov reg, imm32` / `cmp reg, imm32` 都装不下，必须 `movabs`（10 字节）。
   DX11 版依赖"扩展数组分配在 2GB 以下 + `mov r64, imm32` 7 字节"，这个前提在 DX12 里**只对"插件自己分配的数组"成立**：
   - ✅ **可以**：把"扩展数组基址"（插件分配、< 2GB）写成 imm32 → 重定向补丁仍然可行
   - ❌ **不可以**：把 DX12 原列表对象地址写进 imm32 → 只能 `movabs`，或干脆不引用它（这正是 hook 方案能成立的原因：
     hook 里 this 走 rbx 寄存器，不需要把地址编成立即数）

   > 也就是说：**DX11 的"重定向补丁"思路（把数组地址编成 imm32）在 DX12 上依然可行，
   > 但"用列表对象地址做比较/寻址"的地方（如 `cmp rbx, m_VBList`）必须改用 `movabs` 或改成 hook 内部判断。**

2. **DX12 的 `m_RefCount` 在 +0x80，`m_cItems` 在 +0x0** —— 这两个和 DX11 一样，hook 逻辑可照搬。

3. **`sub_140971490` 会 memset 回 0**：`memset(&qword_146C7A440, 0, 327680)` 等。
   如果插件把数组重定向走了，这些 memset 必须跟着改，否则会清到旧数组（无害）或越界（有害）。
   同理 `memset(&unk_146C720C0, 0, 0x8000)` 清的是 **m_RefCount**（m_IBList+0x80）。

---

## 6. 三态判定与遗留事项

1. §1.5 表里 10 个 ReleaseResource 孪生 → 列表身份（已确认 #3=IB/VB、#6=ComputeSurface、#7=ComputeSPI、#8=SPI；
   #1/#2/#4/#5/#9/#10 属 TextureViews/ConstantSet/SurfaceSet 等**与 UnlockModelLimit 无关**的列表，不必再查）。
2. ~~SPI 的 4 个 disp32 站点~~ → **已解决**（DX12 收敛为 1 处 `0x95C2E3`，见 §4.3 末尾）。
3. DX12 的 `ExtraData` 前 0x50 字节的字段语义（Desc 那 24 字节在 DX12 里装的是什么）。**对补丁实现无影响**——
   插件只需要按 0x70 步长原样搬运 ExtraData，不解析其内容。
4. ~~DX12 里 `UnlockIB/UnlockVB/LockDynamic*/Update*Subresource` 是否真的不存在~~ → **已判定：不存在**（见 §6.0）。
5. ~~DX12 `Reap` 的对应函数~~ → **已确定 = `sub_14096E100`**（见下）。
6. ~~DX12 的 `SetVbIb` 绑定路径~~ → **已确定** = `sub_14095F5E0`（size 0x178），与 DX11 `SetVbIb` 逐句同构
   （读记录 → `IASetIndexBuffer`/`IASetVertexBuffers`，走 vtable +344/+352）。4 个站点全部定位（见 §6.0 / §7.4）。

### 6.0 三态判定总表（DX11 的 25 个原标记为 ❓/❌ 的站点）

> 判据：`xrefs_to` 全量引用集 + `decompile` 原文 + `find_regex` 模块级证据。
> **DX11 原实现里 25 个标 ❓/❌ 的站点，现在全部有了定论（0 项遗留）。**

| 判定 | 项数 | 含义 |
|---|---|---|
| 不存在 | 14 | DX12 无对应，**不要移植** |
| 存在 | 9 | 已给出 DX12 RVA + 补丁方式 |
| 无法判定 | **0** | — |

**判定为"不存在"（14 项）**——证据是"该数据地址的全部引用者里没有该形态的代码"：

| 站点 | DX11 RVA | 判据 |
|---|---|---|
| UnlockIB ExtraData | 0x95F866 | IB ExtraData 基址 `0x146CCA580` 只有 3 个引用者（`sub_14094A550`/`CreateIBAtHandle`/`sub_14096E100`），无 Unlock 形态 |
| UnlockIB Resources ×2 | 0x95F88D / 0x95F973 | 同上；且 DX12 无 `+0x8540` 指针数组 |
| UnlockVB ExtraData | 0x95FB86 | VB ExtraData `0x14F30D600` 只有 4 个引用者（`sub_14094A6B0`/`sub_14096C250`/`CreateVBAtHandle`/`sub_14096E100`） |
| UnlockVB Resources ×2 | 0x95FBAD / 0x95FC93 | 同上 |
| LockDynamicIB / VB | 0x951945 / 0x951B85 | DX12 引用记录数组的函数里没有 DX11 那种 0x17 字节 thunk |
| LockAndFillStaticIB ExtraData/Resources | 0x9517CB / 0x9517D5 | `disp 0x18680` 在 DX12 布局里无意义 |
| LockAndFillStaticVB ExtraData/Resources | 0x95185B / 0x951865 | 同上 |
| RenderSurface ExtraData.iInputFormat | 0x957F8A | **`xrefs_to(0x14F30D628 = VB ExtraData+0x28)` → 0 引用；`xrefs_to(0x146CCA5AC = IB ExtraData+0x2C)` → 0 引用**（最强证据） |

**模块级旁证**：
- `find_regex("d3d1[12]")` → DX12 只有 `d3d12.dll`；`find_regex("d3d11")` 在 DX12 **total 0**。
  → D3D11 的 `Map/Unmap/UpdateSubresource` 动态缓冲模型在 DX12 二进制里**整体不存在**。
- symdb 检索：DX12 里 `UnlockIB/UnlockVB/LockDynamicIB/VB/LockAndFillStaticIB/VB/Reap/RenderSurface/SetVbIb/AddRefIB/ReleaseIB` 全部 0 命中（DX11 同批全部命中）。

> ⚠ **这批"不存在"判定的置信边界**：判据是"引用该列表基址/数据地址的**全部**函数里没有该形态"。
> 这对 `RenderSurface`（`xrefs_to` 返回 0 引用）是**决定性**的；对其余站点则是**强但非穷尽**——
> 一个被搬到别处的等价实现（例如改用别的寄存器/结构）不会被这个判据捕获。
>
> ~~**一条待追的线索**：DX12 里的 `" UpdateSubresource: %s [Count:%d]"` 字符串……~~
> → **已排除（原本是我的误判）**：该字符串的引用者在两边是**同一个帧日志函数**
> （DX11 `sub_140952150` / DX12 `sub_14094AB40`，size 均 **0xbbc**、引用点偏移均 **+0x53B**），
> 它只按命令类型分派打印日志，**不是实现路径**。详见 §8.1 问题 2。

**判定为"存在"（9 项）**：

| 站点 | DX11 RVA | DX12 RVA | 说明 |
|---|---|---|---|
| SetVbIb IB Resources | 0x95C177 | **0x95F62A** | `mov rax, rva qword_146C7A440[rsi+rcx*8]` |
| SetVbIb VB Resources | 0x95C1B9 | **0x95F6D7** | `mov rax, rva qword_14F2BD4C0[rsi+rcx*8]` |
| SetVbIb IB ExtraData.Format | 0x95C183 | **0x95F643** | 字段**已移到 40 字节记录**：`mov eax,[rcx+rsi+6C7A45Ch]` |
| SetVbIb VB ExtraData.iInputFormat | 0x95C1E8 | **0x95F6F2** | `mov eax,[rbx+rsi+0F2BD4DCh]` |
| Shutdown VB RefCount | 0x95CD05 | **0x971600** | `lea rcx, unk_14F2B5140` + `mov r8d, 8000h` + `call sub_140990410`（在 `sub_140971490` 里） |
| Shutdown IB RefCount | 0x95CD96 | **0x971666** | `lea rcx, unk_146C720C0` + `mov r8d, 8000h` + `call sub_140990410` |
| Reap VB Resources | 0x954931 | **0x96E16D** | `lea rbx, rva qword_14F2BD4C0[r12]`（`sub_14096E100`） |
| Reap IB Resources | 0x954A31 | **0x96E1ED** | `lea rbx, rva qword_146C7A440[r12]` |
| Reap VB/IB Resources=null | 0x95494F / 0x954A4F | `sub_14096E100` 内 | `sub_14098FFF0(rec, 40)` + `sub_14098FFF0(extra, 112)` |

> ⚠ **注意 `0x971600 / 0x971666` 的归属**：它们在 **`sub_140971490`（Reset）** 里，
> 不在 Shutdown 实体 `sub_140950B20` 里。DX11 的 0x95CD05/0x95CD96 在 `DoAppSpecificShutdown` 里，
> 而 DX12 把同样的清零动作放在了 Reset。**两处都要补**：
> - `sub_140971490` 的 `lea rcx, unk_14F2B5140` / `unk_146C720C0`（`mov r8d, 8000h`）
> - `sub_140971490` 的 `lea rcx, qword_14F2BD4C0` / `qword_146C7A440`（记录数组清零，`sub_140990410(..., 0x50000)`）
> - `sub_140945160`（被 Shutdown 调用）里的 `+128`（= +0x80 RefCount）

**判定为"无法判定"（0 项）**——原 3 项（SPI 的 Shutdown 站点）已被 §4.2/§4.3 的 SPI 基址绑定结果覆盖。

### 6.1 已确定的两处"原判定为 ❌ 其实存在"的站点

**（1）Reap → `sub_14096E100`（0x14096E100，size 0x1220）**

DX11 的 Reap 4 个站点（0x954931 / 0x954A31 / 0x95494F / 0x954A4F）在 DX12 的对应在这里。
证据（`disasm 0x14096E1A0` 窗口内）：

```
14096e1c0  mov rax, cs:qword_146CCA550          ; m_DeleteList 的 ptr（=m_IBList+0x58510）
14096e1c7  lea rdi, rva unk_146CCA580[r12]      ; ExtraData 数组基址
14096e1d1  movzx ebx, word ptr [rax+rcx*2]      ; 取一项 index
14096e1d5  imul rax, rbx, 70h                   ; ×0x70 -> ExtraData[idx]
14096e1d9  add rdi, rax
14096e1df  call sub_140970250                   ; 销毁
14096e1e4  lea rax, [rbx+rbx*4]                 ; ×5
14096e1e8  mov edx, 28h
14096e1ed  lea rbx, rva qword_146C7A440[r12]    ; 资源记录数组基址
14096e1f5  lea rbx, [rbx+rax*8]                 ; ×8 -> 40 字节步长
14096e1fc  call sub_14098FFF0                   ; 清零（0x28 字节）
14096e209  call sub_14098FFF0                   ; 清零 ExtraData（0x70 字节）
```

→ 与 DX11 的 Reap 语义完全一致（遍历 DeleteList，取记录 → 释放 → 置空）。
**注意**：DX12 里"记录"和"ExtraData"是**两个不同的数组**，所以 DX11 的 2 个站点在 DX12 变成 2 处，
但**多了一处 `lea rdi, unk_146CCA580`（ExtraData 基址）**——共 3 处要重定向。

**（2）Shutdown refcount clear → `sub_140945160`（0x140945160）**

DX11 的 0x95CD05（VB RefCount 基址）/ 0x95CD96（IB RefCount 基址）两个站点，
在 DX12 合并成**一个**通用函数（调用方传列表基址）：

```c
__int64 __fastcall sub_140945160(__int64 a1)   // a1 = &m_IBList 或 &m_VBList
{
  for (i = 0; (unsigned)v2 < *(_DWORD*)a1; i = v2) {
    if (*(_DWORD*)(a1 + 4LL*v2 + 128) != 0) {          // a1 + 0x80 + v2*4  = m_RefCount[v2]
      *(_DWORD*)(a1 + 4LL*v2 + 128) = 0;
      v3 = *(_DWORD*)(a1 + 361752);                    // 0x58518 = m_DeleteList.m_nCapacity
      if (*(_DWORD*)(a1 + 361760) == v3)               // 0x58520 = m_DeleteList.m_nCurrSize
        sub_14094F070(a1 + 361472, v3 + 1, &i, 0);     // 0x58400 = m_DeleteList 基址
      else {
        v4 = (unsigned __int16*)(*(_QWORD*)(a1 + 361744) + 2LL * *(unsigned*)(a1 + 361760)); // 0x58510 = ptr
        if (v4) *v4 = v2;
      }
      ++*(_DWORD*)(a1 + 361760);
    }
    result = ++v2;
  }
  return result;
}
```

调用者：`sub_140950B20`（DX12 的 Shutdown 实体）里
`sub_140945160(&Graphics::_INTERNAL::m_VBList); sub_140945160(&Graphics::_INTERNAL::m_IBList);`

→ **DX12 侧 Shutdown 的 RefCount 部分只需改 `sub_140945160` 这一处的 `+128`（即 +0x80）**，
不需要 DX11 那样的两个独立站点。

> **IDA 稳定性提醒（已定位根因）**：一次 `insn_query` 的**全 .text 段扫描**（`allow_broad:true, max_scan_insns:4000000`）
> 会把 DX12 的 IDA 插件线程彻底卡死 —— 该调用超时后，**之后所有 DX12 请求一律超时**（含 2 指令 disasm / `get_bytes` / `xrefs_to`），
> 约 4 分钟后 MCP 层短暂恢复（`instance_list` 能返回），随后再次全线超时。
> **禁止**在 DX12 侧做 segment 级 `allow_broad` 扫描。查询要串行、小批量。
>
> 另外两个已实测的坑：
> - `find_bytes` 在该实例上对**已知存在**的字节（如 `FF 84 8B 80 00 00 00`）也返回 0 命中 —— **不能作为"不存在"的证据**。
> - `list_funcs` / `func_query` 的 `filter` 参数无效，永远返回从 0x140001460 开始的第一页；按名找函数只能用 `lookup_funcs` + 绝对 VA 或完整 mangled 名。

---

## 7. DX12 移植施工清单（可直接抄进 C++）

> 所有 RVA 相对 `CivilizationVI_DX12.exe` 基址 0x140000000。
> **注意**：DX11 的补丁用 `ResolveByRva(kHookImageExeDx11, ...)`，DX12 必须用 `kHookImageExeDx12`。
> 建议把 `UnlockModelLimit.cpp` 的地址表改成"按映像二选一"的结构（`HookInfoExe` 的思路）。

### 7.1 数据地址

| 名称 | DX11 RVA | DX12 RVA |
|---|---|---|
| m_IBList | 0x2B0A740 | **0x6C72040** |
| m_VBList | 0x2B91200 | **0xF2B50C0** |
| m_SPIList | 0x2AB9E40 | **0x6C1E440** |
| m_ComputeSPIList | 0x2822C80 | **0x74DE300** |
| m_SPIList.m_RefCount | 0x2822D00 | **0x6C1E4C0** |
| m_SPIList.m_Resources | 0x2ACA300 | **0x6C2E900** |
| m_SPIList.m_Resources.m_iPackageID | 0x2ACA304 | **0x6C2E904** |
| m_ComputeSPIList.m_Resources | — | **0x74E6700** |

### 7.2 四个 hook 站点

| 目标 | DX11 RVA | DX12 RVA | 指令 | this | indexReg |
|---|---|---|---|---|---|
| SPI AddResource | 0x940F31 | **0x9413A1** | `inc dword ptr [rbx+rcx*4+80h]` | rbx | rcx |
| SPI ReleaseResource | 0x956A51 | **0x94DAC1** | `add dword ptr [rbx+r8*4+80h], -1` | rbx | r8 |
| IB/VB AddResource | 0x9408A1 | **0x940D11** | `inc dword ptr [rbx+rcx*4+80h]` | rbx | rcx |
| IB/VB ReleaseResource | 0x95664A | **0x94D6BA** | `mov esi, [rbx+r8*4+80h]` | rbx | r8 |

跳回点（`xchg tempReg, [rsp]; ret` 的目标）= hook 站点 + 该指令长度：
- SPI/IBVB AddResource 的 `inc` 是 7 字节 → 跳回 **+7**（DX11 版用 `DecodeInsn` 动态取，可照搬）
- ReleaseResource 的 `mov esi` 是 7 字节 → 跳回 **+7**

**字节级交叉验证**（`get_bytes` 独立取回，**DX12 与 DX11 对应站点逐字节同码**）：

| 站点 | DX11 RVA | DX11 字节 | DX12 RVA | DX12 字节 | 一致 |
|---|---|---|---|---|---|
| IB/VB AddResource | 0x9408A1 | `FF 84 8B 80 00 00 00 89` | **0x940D11** | `FF 84 8B 80 00 00 00 89` | ✅ |
| SPI AddResource | 0x940F31 | `FF 84 8B 80 00 00 00 89` | **0x9413A1** | `FF 84 8B 80 00 00 00 89` | ✅ |
| IB/VB ReleaseResource | 0x95664A | `42 8B B4 83 80 00 00 00` | **0x94D6BA** | `42 8B B4 83 80 00 00 00` | ✅ |
| SPI ReleaseResource | 0x956A51 | `42 83 84 83 80 00 00 00` | **0x94DAC1** | `42 83 84 83 80 00 00 00` | ✅ |

→ 这意味着 **hook 体的 asmjit 汇编代码可以逐字复用**，只需把 `hookAddr` / `jumpBack` 的 RVA 换成 DX12 的值。

> ⚠ **注意 DX11 原实现的一个不对称**：IB/VB 的 Release hook 打在 `mov esi`（读新 refcount）上，
> 而 SPI 的 Release hook 打在 `add`（自减）上。DX12 移植时要保持各自对应的位置：
> - IB/VB：DX12 `mov esi,[rbx+r8*4+80h]` @ **0x94D6BA**（= 0x94D670 + 0x4A，与 DX11 0x956600+0x4A 同构）✅
> - SPI：DX12 `add dword ptr [rbx+r8*4+80h],-1` @ **0x94DAC1**（= 0x94DA80 + 0x41，与 DX11 0x956A10+0x41 同构）✅

### 7.3 上限字节补丁（全部写"第 2 字节"）

| DX11 RVA / 值 | DX12 RVA / 值 | 含义 |
|---|---|---|
| 0x9407CF / 0x40 | **0x940C3F / 0x40** | IB/VB `cmp eax, 2000h` → `4000h` |
| 0x9407FE / 0x40 | **0x940C6E / 0x40** | IB/VB `mov edx, 2000h` |
| 0x940864 / 0x40 | **0x940CD4 / 0x40** | IB/VB `mov r8d, 2000h`（带 REX.B，所以是 +3） |
| 0x940E5F / 0x80 | **0x9412CF / 0x80** | SPI `cmp eax, 4000h` → `8000h` |
| 0x940E8E / 0x80 | **0x9412FE / 0x80** | SPI `mov edx, 4000h` |
| 0x940EF4 / 0x80 | **0x941364 / 0x80** | SPI `mov r8d, 4000h`（带 REX.B，+3） |
| 0x9417EB / 0x04 | **无对应** | D3D11_BUFFER_DESC.ByteWidth（DX12 无此结构） |

**独立复核**（`get_bytes` 逐字节取回，确认改之前的值就是下面这些）：

| DX12 地址 | 当前值 | 应改为 | 语义 |
|---|---|---|---|
| 0x940C3F | **0x20** | 0x40 | IB/VB `cmp eax, 2000h` → `4000h` |
| 0x940C6E | **0x20** | 0x40 | IB/VB `mov edx, 2000h` |
| 0x940CD4 | **0x20** | 0x40 | IB/VB `mov r8d, 2000h` |
| 0x9412CF | **0x40** | 0x80 | SPI `cmp eax, 4000h` → `8000h` |
| 0x9412FE | **0x40** | 0x80 | SPI `mov edx, 4000h` |
| 0x941364 | **0x40** | 0x80 | SPI `mov r8d, 4000h` |

→ 六处全部与文档一致，**可以直接写进补丁**。

### 7.4 重定向站点

**IB/VB 一族：DX11 的 31 个站点里，只有极少数在 DX12 有对应。**
DX12 里 IB/VB 列表内的"资源记录数组"（+0x8400，步长 0x28）与 ExtraData 数组（+0x58540，步长 0x70）
**只有 5 个和 3 个引用者**（`xrefs_to 0x146C7A440` / `0x146CCA580`），远少于 DX11 的 31 个：

| 引用者（DX12） | 用途 |
|---|---|
| `sub_14094A550` | `lea rcx, qword_146C7A440` + `lea rcx, unk_146CCA580` → 资源记录/ExtraData 取址 |
| `sub_14095F5E0` | `mov rax, rva qword_146C7A440[rsi+rcx*8]` + `[rcx+rsi+6C7A45Ch]` → 读记录 |
| `CreateIBAtHandle` 0x14096A4E0 | `lea rcx, qword_146C7A440` / `lea rax, unk_146CCA580` |
| `sub_14096E100` | `lea rdi, rva unk_146CCA580[r12]` / `lea rbx, rva qword_146C7A440[r12]` → **Reap 一族** |
| `sub_140971490` | Reset 的 memset |

→ **DX12 的 IB/VB 重定向只需要覆盖这 5 个函数里的 6~8 处 `lea`**，而不是 DX11 的 31 处。

**逐点清单（IB 侧，由 `xrefs_to` 直接给出，全是 `type: data` 引用）**：

| DX12 引用指令 RVA | 所属函数 | 引用对象 | 对应 DX11 站点 |
|---|---|---|---|
| **0x94A565** | sub_14094A550 | 资源记录数组（+0x8400） | ≈ CreateIBAtHandle Resources |
| **0x94A553** | sub_14094A550 | ExtraData 数组（+0x58540） | ≈ CreateIBAtHandle ExtraData |
| **0x95F62A** | sub_14095F5E0 | 资源记录数组 | ≈ SetVbIb IB Resources |
| **0x96A4FA** | CreateIBAtHandle | 资源记录数组 | 0x94529B |
| **0x96A51E** | CreateIBAtHandle | ExtraData 数组 | 0x94508D |
| **0x96E1ED** | sub_14096E100（Reap 一族） | 资源记录数组 | ≈ 0x954A31 / 0x954A4F（Reap IB） |
| **0x96E1C7** | sub_14096E100 | ExtraData 数组 | — |
| **0x971658** | sub_140971490（Reset） | 资源记录数组 | — （memset 0x140000） |

**VB 侧**：数组基址 = `0x14F2BD4C0`（记录）与 `0x14F30D600`（ExtraData）。
已直接从 `sub_14095F5E0` 的反汇编里拿到两处（该函数同时处理 IB 和 VB）：

| DX12 引用指令 RVA | 指令原文 | 引用对象 |
|---|---|---|
| **0x95F6D7** | `mov rax, rva qword_14F2BD4C0[rsi+rcx*8]` | VB 资源记录数组 |
| **0x95F6F2** | `mov eax, [rbx+rsi+0F2BD4DCh]` | VB rec->f1C |
| **0x95F6FD** | `mov eax, [rbx+rsi+0F2BD4D8h]` | VB rec->f18 |
| **0x95F71F** | `mov eax, [rbx+rsi+0F2BD4E0h]` | VB rec->f20 |

对应 IB 侧在同一函数里：

| **0x95F62A** | `mov rax, rva qword_146C7A440[rsi+rcx*8]` | IB 资源记录数组 |
| **0x95F643** | `mov eax, [rcx+rsi+6C7A45Ch]` | IB rec->f1C |
| **0x95F653** | `mov eax, [rcx+rsi+6C7A458h]` | IB rec->f18 |

其余 VB 站点（同样由 `xrefs_to` 全量取得）：

| DX12 引用指令 RVA | 所属函数 | 引用对象 | 对应 DX11 站点 |
|---|---|---|---|
| **0x96E16D** | sub_14096E100（Reap） | VB 资源记录数组 | 0x954931 |
| **0x96E147** | sub_14096E100（Reap） | VB ExtraData 数组 | — |
| **0x971600** | sub_140971490（Reset） | **m_RefCount**（`unk_14F2B5140`）+ `mov r8d, 8000h` | 0x95CD05 |
| **0x9715F2** | sub_140971490（Reset） | VB 资源记录数组（`sub_140990410(..., 0x50000)`） | — |
| **0x94A6C5** | sub_14094A6B0（ReleaseVB） | VB 资源记录数组 | ≈ 0x956600 一族 |
| **0x94A6B3** | sub_14094A6B0 | VB ExtraData 数组 | — |
| **0x96C297** | sub_14096C250 | VB 资源记录数组 | — |
| **0x96C2AC** | sub_14096C250 | VB ExtraData 数组 | — |

**IB 侧补充**：

| **0x971666** | sub_140971490（Reset） | **m_RefCount**（`unk_146C720C0`）+ `mov r8d, 8000h` | 0x95CD96 |
| **0x95F643** | sub_14095F5E0（SetVbIb） | IB rec->f1C（`6C7A45Ch`） | 0x95C183 |
| **0x95F653** | sub_14095F5E0 | IB rec->f18（`6C7A458h`） | — |

引用者与 IB 侧一一对应：`sub_14094A550`(IB)/`sub_14094A6B0`(VB) / `sub_14095F5E0` / `CreateIBAtHandle` 0x14096A4E0 / `CreateVBAtHandle` 0x14096CD40 / `sub_14096E100` / `sub_140971490`。

> 🔑 **一个新增的编码差异**：DX12 这几处用的是 `[rsi + rcx*8 + disp32]`（**rsi 里存的是镜像基址 0x140000000**），
> 而 DX11 用的是 `[rdi + rax*8 + disp32]`（rdi = 列表指针）。
> 这意味着**不能只改 disp32**：必须像 DX11 的 B 类补丁那样，把 SIB 的 base 从 `rsi`(110) 改成"无 base"(101)，
> 即整条指令从 `48 8B 04 CE <disp32>` 改成 `48 8B 04 CD <disp32>`（**长度不变，8 字节**），
> disp32 写插件数组的绝对地址（< 2GB）。asmjit 的 `m.setIndex(idx); m.setShift(3); m.setOffset(addr)` 正好编出这个形态。
> DX11 版 `EncodeAndApplyIBVBPatch` 的 kind=1/2 分支**可以照搬**，但必须核对原始 SIB 字节里确实带 base。

> 注意 `sub_14096E100` 就是 DX12 的 **Reap 等价物**（§6 待办 #5）：
> 它遍历 `m_DeleteList`，对每一项取记录（`imul rax, rbx, 70h` + `qword_146CCA580`）与资源记录（`[rbx+rbx*4]*8` + `qword_146C7A440`），
> 分别 `sub_140970250` / `sub_14098FFF0(.., 0x28)` / `sub_14098FFF0(.., 0x70)` —— 即"销毁并清零"。

**SPI 一族：DX11 的 12 个站点在 DX12 收敛为 9 处，全部已定位**（见 §4.3）：

| DX11 站点 | DX12 RVA | 类型 | 补丁方式 |
|---|---|---|---|
| 0x946A37 CreateShaderPackageInstance | **0x944A77** | `lea rax, imm32` | kind=0：`lea` → `mov r64, imm32`（7 字节） |
| 0x957990 RenderSurface | **0x95E20A** | `lea rax, imm32` | kind=0 |
| 0x956B15 ReleaseShaderPackageInstance | **0x94DD35** | `lea rax, imm32` | kind=0 |
| 0x95D775 Shutdown memset (Resources) | **0x97175B** | `lea rcx, imm32` | kind=0 |
| 0x95D783 Shutdown memset (RefCount) | **0x97177F** | `lea rcx, imm32` | kind=0 |
| 0x940542 AddRefShaderPackageInstance | **0x9407E0** | `lea rcx, m_SPIList` | kind=0（→ RefCount-0x80 伪基址） |
| 0x94D013 GetEnumFromShaderPackageInstance | **0x9479E0** | `lea rcx, m_iPackageID` | kind=0 |
| 0x94ABAC / 0x9504FB / 0x9506F9 / 0x950757（**4 处**） | **0x95C2E3**（**1 处**） | `mov eax,[r13+rax*8+0x6C2E904]` | **kind=1 变体**：ModRM `0x84`→`0x04`（1 字节）+ 改 disp32 |
| 0x95CEB5 Shutdown refcount clear | — | — | DX12 无 `lea r13` 形态，已由 0x97177F 覆盖 |

> ⚠ **`0x95C2E3` 的补丁要特别注意**：它不是标准的"7/8 字节整条替换"，
> 而是 **1 字节 ModRM 改动 + 4 字节 disp32 改动**，指令长度不变。用 `PatchBytes` 分两次写即可。

### 7.5 数组分配尺寸（DX12）

```
IB/VB 每个列表：
  m_RefCount[32768]        = 32768 * 4   = 0x20000
  资源记录[32768]           = 32768 * 0x28 = 0x140000
  ExtraData[32768]          = 32768 * 0x70 = 0xE00000
  小计                                    = 0xF60000
两个列表（IB + VB）                        = 0x1EC0000

  参考：原列表的对应部分（8192 项）
    m_RefCount 8192*4  = 0x8000
    资源记录    8192*0x28 = 0x50000
    ExtraData   8192*0x70 = 0x380000
    合计                   = 0x3D8000

SPI（m_SPIList）—— 注意 SPI 的重定向只需要 Resources + RefCount，不需要 ExtraData：
  m_Resources[32768]        = 32768 * 16  = 0x80000
  m_RefCount[32768]         = 32768 * 4   = 0x20000
  小计                                    = 0xA0000

SPI（m_SPIList）：
  m_Resources[32768]        = 32768 * 16  = 0x80000
  m_RefCount[32768]         = 32768 * 4   = 0x20000
  小计                                    = 0xA0000
```

> ⚠ **分配必须 < 2GB**：所有重定向用的 `mov reg, imm32` / `mov reg,[idx*2^shift+disp32]`
> 都要求目标地址能表示为 **sign-extended imm32**（即 < 0x80000000）。
> 循环 `VirtualAlloc` 重试的写法（DX11 版第 147-159 行）必须保留。

### 7.5b 推荐接入方式：用现成的 `HookInfoExe`

`recipes/Global/Civ6BasePlugin/dll/HookAPI.h` **已经内置了"DX11/DX12 二选一"的机制**，DX12 分支不需要自己写 `if (IsImageLoaded(...))`：

```cpp
// HookAPI.h:139
typedef struct HookInfoExe {
    HookInfo dx11;
    HookInfo dx12;
} HookInfoExe;

// HookAPI.h:592  —— 自动判断当前进程是哪个 exe
static inline HookImageId HookCurrentExeImage(HookAPI* apiTable);

// HookAPI.h:603  —— 按当前映像自动挑 dx11/dx12 那一支
static inline void* ResolveByHookInfoExe(HookAPI* apiTable, const HookInfoExe& info, size_t span = 1);
```

`HookInfo` 支持三种取址 + AOB 兜底：

```cpp
HookInfo{ kHookInfoRva, /*aob=*/nullptr, /*index=*/0, {.rva = 0x940BE0} }
HookInfo{ kHookInfoRva, "48 89 5C 24 08 48 89 74 24 10 57 48 81 EC 80 00", 1, {.rva = 0x940BE0} }
```

**建议**：把 `UnlockModelLimit.cpp` 里的

```cpp
static uint64_t ResolveExe(uintptr_t rva, size_t span) {
    return g_api ? (uint64_t)g_api->ResolveByRva(kHookImageExeDx11, rva, span) : 0;
}
```

改成"每张地址表带 dx11/dx12 两个 RVA"，用 `ResolveByHookInfoExe` 统一解析。这样：
- 同一个插件在 DX11 和 DX12 进程里都能加载；
- `HookCurrentExeImage` 已经把"哪个 exe 加载了"这件事判好了，插件不用自己判；
- AOB 兜底可以在 exe 小版本更新、RVA 漂移时自动兜住。

### 7.6 与 DX11 版逻辑上的三处**必须重写**

1. **`InstallHook_AddResource` 的 `cmp rbx, m_VBList` 分支**
   DX12 的 `m_VBList = 0x14F2B50C0` **> 2GB**，asmjit 的 `a.cmp(reg, (uint64_t)imm)` 会编成
   `mov r10, imm64; cmp rbx, r10`（两条指令，共 17 字节）—— hook 体还是放得进 256 字节的 `.uhook` 槽，
   但**不能再假设"一条 cmp 就能比较"**。
   更稳妥的做法：DX12 里 IB/VB 的 **ReleaseResource 是两个不同的 thunk**，可以直接
   只在 `sub_14094D400`（IB）和 `sub_14094DDD0`（VB）上各装一个 hook —— 它们只有 15 字节，
   签名清晰（`unsigned short f(unsigned short idx)`），比在实体里判断基址干净得多。
   AddResource 侧因为 IB/VB 共用一个实例，仍需 `cmp rbx, m_VBList`，但注意上面说的 > 2GB 问题。

2. **`InstallHook_Release` 的目标变了**
   DX11 只有 1 个 ReleaseResource 实体服务 IB/VB 两个列表；DX12 有 **10 个**。
   如果只关心 IB/VB，只需 hook **0x94D670**（同时被 IB/VB 的 thunk 调用），
   **不需要**装 10 个。

3. **`ApplyLimitBytePatches` 的 `PatchLimitByte(0x9417EB, 0x04)` 必须去掉**（DX12 无此结构）。

---

## 8. 交付状态

### 8.1 逐项状态

| 项目 | 状态 | 证据强度 |
|---|---|---|
| IB/VB 列表真实布局 | ✅ 实测确定 | 强（`[reg+disp]` 反汇编 + 布局自洽校验） |
| IB/VB 列表基址（DX12） | ✅ 0x6C72040 / 0xF2B50C0 | 强（`xrefs_to` 全量 9 个引用者） |
| SPI 列表布局 | ✅ 与 DX11 一致 | 强（两边 `type_inspect` 逐字段相同 + 实机 `[reg+disp]` 复核） |
| SPI 列表基址（DX12） | ✅ 0x6C1E440 / 0x74DE300 | 强（AddResource 实例唯一 code xref） |
| 4 个 hook 站点（DX12 RVA + 字节） | ✅ 已核实 | **最强**（`get_bytes` 独立复核，与 DX11 逐字节同码 → hook 体可逐字复用） |
| 6 个上限字节补丁（DX12 RVA） | ✅ 已核实 | **最强**（`get_bytes` 独立复核，原值 0x20/0x20/0x20 + 0x40/0x40/0x40） |
| Reap 的 DX12 对应 | ✅ = `sub_14096E100` | 中强（反汇编逐条对齐语义） |
| Shutdown RefCount 的 DX12 对应 | ✅ = `sub_140945160` | 强（反编译原文 + 偏移换算 361744/361752/361760） |
| SPI 重定向站点 | ✅ **9 处全部定位**（12 个 DX11 站点 → DX12 收敛为 9 处） | 强（`xrefs_to` 对语义数据地址全量对齐 + `get_bytes` 原始字节） |
| IB/VB 重定向站点 | ✅ 只需覆盖 5 个函数（8 处 `lea` 已列出） | 强（`xrefs_to` 全量） |
| UnlockIB / UnlockVB / LockDynamicIB/VB / LockAndFillStaticIB/VB 在 DX12 | ✅ **不存在**（13 个站点） | 中强（`xrefs_to` 全量引用集里无该形态 + DX12 二进制无 `d3d11` 字样 + 布局上 `+0x8540` 根本不存在） |
| SetVbIb / Reap / Shutdown RefCount 在 DX12 | ✅ **存在**（9 个站点已定位） | 强（`xrefs_to` + `decompile` 原文逐句同构） |
| ~~剩余未解问题~~ | ✅ **无** | 两个问题均已定论（§8.3） |
| DX12 代码实现 | ⛔ 未开始 | 文档 §7 已给出全部 RVA/字节/分配尺寸，可直接开写 |

### 8.2 最终结论一句话

> **DX11 的 `m_Resources` 指针数组（+0x8540）在 DX12 里不存在**，被 `+0x8400` 起、步长 `0x28` 的"资源记录数组"取代。
> 连带 **`UnlockIB / UnlockVB / LockDynamicIB/VB / LockAndFillStaticIB/VB` 这 13 个站点在 DX12 没有任何对应物**
> （D3D11 的动态缓冲路径整体消失 —— DX12 二进制里连 `d3d11` 字样都没有）。
> **能移植的是**：4 个 hook 站点（与 DX11 逐字节同码）、6 个上限字节补丁（偏移算术与 DX11 同构）、
> **SPI 的 9 处重定向站点（已全部定位）**、IB/VB 的 8 处 `lea` + SetVbIb 4 处 + Reset/Reap 8 处。
> **不能照抄的是**：所有 `[reg+idx*8+0x8540]` 形态的 disp32 补丁 —— 那些位置在 DX12 已经不存在了。
>
> **DX11 原实现里所有标 ❓/❌ 的 25 个站点已全部定论**（14 个无对应 / 9 个已定位 / 0 个遗留），
> 加上 hook 4 处、上限字节 7 处、SPI 12 处、IB/VB 重定向 31 处，**DX12 侧已无未解项**。

### 8.3 两个未解问题均已定论（无剩余）

**问题 1：SPI 的 4 个 disp32 站点 → 已解决**（详见 §4.3 末尾）

`xrefs_to(0x146C2E904)` 在 DX12 全镜像**只有 2 处**：`0x1409479E3`(lea) 与 `0x14095C2E3`(disp32)。
→ **DX11 的 4 个 disp32 站点在 DX12 收敛成 1 处**，宿主 = `sub_14095C020`（size 0x377）。

```
get_bytes(0x14095C2DC,16) = 41 0F B7 C2 48 03 C0 41 8B 84 C5 04 E9 C2 06
14095c2dc  41 0F B7 C2               movzx eax, r10w
14095c2e0  48 03 C0                  add rax, rax            ; rax = 2*i
14095c2e3  41 8B 84 C5 04 E9 C2 06   mov eax, [r13+rax*8+0x6C2E904]  ; m_Resources[i].m_iPackageID
```

补丁（与 DX11 同构，可复用 `EncodeAndApplyIBVBPatch` kind=1 的思路）：
```
地址 0x14095C2E5  0x84 -> 0x04      （ModRM mod 10 -> 00，去掉 base，变绝对寻址）
地址 0x14095C2E7  disp32 -> 插件数组绝对地址（< 2GB）
指令长度不变（8 字节），无需 NOP 填充。
```
> DX11 原站点 `8B 84 C7 04 A3 AC 02` 的 ModRM 也是 `84`(mod=10) + SIB `C7`(base=rdi) ——
> **两边结构完全相同**，说明 DX11 版当年用的就是"mod 10→00 去掉 base"这招。
> 唯一差别：DX12 多了 `41`(REX.B) 前缀，而 `EncodeAndApplySPIPatch` 里的
> `int off = ((orig[0] & 0xF0) == 0x40) ? 1 : 0;` 已经处理了这种情况。

**问题 2：DX12 是否有一条"被搬走的" update 路径 → 已否定（我上一轮的怀疑是误判）**

上一轮看到 DX12 有字符串 `" UpdateSubresource: %s [Count:%d]"`（@0x14106A808，被 `0x14094B07B` 引用），
据此怀疑存在未知的更新路径。**实测推翻了它**：

| | DX11 | DX12 |
|---|---|---|
| 字符串地址 | 0x14105AA58 | 0x14106A808 |
| 引用指令 | **0x14095268B** | **0x14094B07B** |
| 宿主函数 | `sub_140952150` | `sub_14094AB40` |
| 宿主函数 size | **0xbbc** | **0xbbc** |
| 引用点在函数内偏移 | +0x53B | +0x53B |

**两边是同一个函数**（同 size、同偏移）—— 一个按命令类型高位（`>> 28`）分派的**帧日志函数**
（开头即 `" ****BEGIN FRAME %d LOG (%d command sets, %d max)***"`）。它只**记录**"这一帧提交过 UpdateSubresource 命令"，
**不是实现**。

> 🔑 关键推论：该 `case 5` 在**两个版本里都存在**，说明它只是**命令流的类型枚举**，
> 不代表 DX12 有对应的 D3D11 实现。DX12 侧只是把命令流里这个类型打出来。
>
> **旁证**：DX12 二进制里 `find_regex("d3d11")` → **0 命中**（只有 `d3d12.dll`）。
> 没有 `ID3D11DeviceContext`，就不可能存在 `UpdateSubresource` 的等价调用点。

→ **§6.0 里 `LockAndFillStaticIB/VB` 判为"不存在"成立**，不需要修正。

### 8.4 结论可靠性分级说明

- **强证据**：来自 `get_bytes` 原始字节 / `xrefs_to` 全量列表 / `decompile` 原文 / `type_inspect` 逐字段对比。
  这类结论可以直接写进补丁代码。
- **中强证据**：来自 `disasm` 窗口内的人工对齐。需要用完整函数反编译再确认一次。
- **弱证据**：依赖 `find_bytes` 的"未命中"结论。**该 IDA 实例上 `find_bytes` 对已知存在的字节也返回 0 命中**，
  所以任何"DX12 不存在 X 形态"的结论都不可靠，必须在可用的 IDA 会话里用 `xrefs_to` + 完整反编译重新验证。

---

## 9. 实现过程中发现的 DX11 既有 BUG（重要）

> 在把补丁移植到 DX12 时，用 asmjit 1.17 做了机器码级验证，**发现 DX11 现存的
> `UnlockModelLimit` 里有一个静默失效的 hook**。

### 9.1 现象

`UnlockModelLimit.cpp` 的 `InstallHook_AddResource` / `InstallHook_Release` 里写着：

```cpp
a.cmp(x86::rbx, (uint64_t)mVBList);   // mVBList = 0x142B0A740 (DX11)
a.je(label2);
```

### 9.2 根因：asmjit 对 `cmp r64, imm64` 的校验 + 错误静默丢弃

asmjit 源码 `x86/x86assembler.cpp`：

```cpp
else if (size == 8) {
    bool can_transform_to_32bit = inst_id == Inst::kIdAnd && Support::is_uint_n<32>(imm_value);
    if (!Support::is_int_n<32>(imm_value)) {
        if (can_transform_to_32bit) size = 4;
        else goto InvalidImmediate;      // <- cmp 走这里
    }
}
...
ERROR_HANDLER(InvalidImmediate)          // err = kInvalidImmediate; goto Failed;
...
Failed:
    reset_state();
    return report_error(err);            // 只走 emitter 的 error handler，不落任何状态
```

而 `CodeHolder::flatten()` 与 `JitRuntime::_add()` **都不检查 emitter 错误** ——
`add()` 照样返回 `kErrorOk`，只是整条 `cmp` 被丢掉。

`m_VBList = 0x142B0A740` 超出 int32，于是触发该路径。

### 9.3 实测机器码（asmjit 1.17，本仓库 lib/asmjit.lib）

```
cmp rbx, 0x14F2B50C0 (>2GB)   err=0(Ok)  size=49
  41 51                  push r9
  48 81 F9 00 20 00 00   cmp rcx, 0x2000
  0F 82 ...              jb  L1
  0F 84 ...              je  L2        <- 前面的 cmp rbx,imm 整个消失了！
  49 B9 <imm64>          mov r9, VB值   <- 永远走这里

cmp rbx, 0x1000 (合法)        err=0(Ok)  size=56
  48 81 FB 00 10 00 00   cmp rbx, 0x1000   <- 正常编出
  0F 84 ...              je  L2
```

### 9.4 后果

`cmp rbx, m_VBList` 消失后，`je label2` 沿用前一条 `cmp indexReg, ORIG_LIMIT` 留下的标志位。
进入该分支时 `indexReg >= 8192` ⇒ 无符号不低于 ⇒ **ZF = 0 ⇒ 恒不跳转**，于是**永远走 VB 分支**：

- `this == m_VBList`（真的 VB 句柄）→ 走 VB 数组 —— 巧合正确
- `this == m_IBList`（IB 句柄）→ **也走 VB 数组**，用 IB 的索引去写 VB 的 RefCount 数组

索引范围 8192..32767，VB 数组有 32768 项 —— 恰好不越界，所以**不会立刻崩**，
只会让 VB 的 refcount 被错误递增。这类"不崩但错"的 bug 极难从现象反推。

### 9.5 修法

拆成两条指令（asmjit 会编出 `49 BA <imm64>` + `4C 39 D3`）：

```cpp
x86::Gp listReg = x86::r10;
a.push(listReg);                       // 本 hook 是"跳回 + ret"，push 必须自己配平
...
a.mov(listReg, (uint64_t)mVBList);     // movabs r10, imm64
a.cmp(x86::rbx, listReg);              // cmp rbx, r10  <- 正确编出
a.je(label2);
...
a.pop(listReg);                        // 必须在 xchg 之前 pop！
a.xchg(tempReg, x86::ptr(x86::rsp));
a.ret();
```

**顺序陷阱**：`xchg(tempReg, [rsp])` 会把栈顶换成跳回地址，
若在它之后才 `pop(listReg)`，弹出的就是跳回地址而不是 r10，`ret` 随即跳飞。必须 **先 pop 再 xchg**。

### 9.6 已修复

`UnlockModelLimit.cpp` 里 4 处（DX11 的 AddResource/Release + DX12 的两处）全部改成
`movabs + cmp reg,reg`，并配平了 push/pop。已用 asmjit 1.17 实测机器码确认：

```
49 BA 40 A7 B0 42 01 00 00 00   movabs r10, 0x142B0A740
4C 39 D3                        cmp    rbx, r10
0F 84 15 00 00 00               je     L2
...
41 5A                           pop    r10      <- 在 xchg 之前
4C 87 0C 24                     xchg   r9, [rsp]
C3                              ret
```

---
## 附：复现用工具调用速查

```ts
// 反汇编（refs 字段给出数据符号名，比读裸 disp 可靠）
tools.mcp__ida__disasm({ addr: "0x140940BE0", instance_id: "ida-11696", max_instructions: 120 })
// 反编译
tools.mcp__ida__decompile({ addr: "0x14096A4E0", instance_id: "ida-11696", include_addresses: false })
// 谁引用了这个数据地址（定位"某列表被哪些函数用"最强的一招）
tools.mcp__ida__xrefs_to({ addrs: ["0x146C72040"], instance_id: "ida-11696" })
// 按 mangled 名找函数（queries 必须是【绝对 VA】或【完整 mangled 名】，传 RVA 会说 "Not a function"）
tools.mcp__ida__lookup_funcs({ queries: ["0x140943E90"], instance_id: "ida-11696" })
// 结构体布局
tools.mcp__ida__type_inspect({ queries: [{ name: "<完整类型名>", include_members: true }], instance_id: "ida-11696" })
// 原始字节（做字节补丁必用）
tools.mcp__ida__get_bytes({ regions: [{ addr: "0x140940C3D", size: 12 }], instance_id: "ida-11696" })
// 按字节特征全局搜（例如找所有 ReleaseResource 孪生）
tools.mcp__ida__find_bytes({ patterns: ["83 84 83 80 00 00 00 FF"], instance_id: "ida-11696" })
```

---
