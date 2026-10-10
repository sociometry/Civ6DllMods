# Civ6DllLoader 插件开发指南

面向：**开发文明 6 C/C++ 功能插件的 Mod 作者**。无需阅读或修改 Loader 源码；只需在工程中包含 `HookAPI.h`，按规范声明插件类别并实现 `OnLoad`，编译后将 DLL 放入对应的 Mod 插件子目录。

---

## 1. 核心架构与运行机制

文明 6 原生只允许在 `Config.sql` 里指定单一 GameCore DLL，因此多个改 GameCore 的 Mod 彼此互斥。Civ6DllLoader 采用双轨架构实现多 Mod 共存：

```text
[稳态运行（已安装 version.dll）]
游戏启动
  → 加载游戏根目录 version.dll（静态导入劫持）
  → 启动 Civ6DllLoader.runtime.dll（Phase A：常驻 exe 域）
  → 挂钩 GameCore::LoadDLL
  → 引擎请求加载 GameCore 时，detour 自动重定向回官方原版 GameCore DLL（薄壳永不加载）
  → 接管 GameCore 生命周期（Phase B），自动扫描并加载各 Mod 的插件 DLL

[首次引导（未安装 version.dll）]
玩家安装带薄壳的 Mod 进入游戏
  → 引擎槽位加载薄壳 GameCore_XP2_Shell_FinalRelease.dll
  → 薄壳扫描已启用 Mod，竞选并启动版本最高的 runtime.dll
  → 自动将自身接管安装为游戏根目录的 version.dll（外来 dll 备份接管）
  → 提示玩家重启游戏，后续即进入稳态运行
```

### 交付文件分工

| 文件 | 来源 | Mod 中的放置与职责 |
|------|------|-------------------|
| `GameCore_XP2_Shell_FinalRelease.dll` | Loader 发行包（薄壳） | 原样放进 Mod 的 `Binaries/Win64/`，用于未安装 `version.dll` 时的首次引导与自动接管 |
| `Civ6DllLoader.runtime.dll` | Loader 发行包（运行时核心） | 原样放进 Mod 的 `Binaries/Win64/`，多 Mod 共存时自动按版本号最高者竞选启动 |
| `Plugins/Global/*.dll` 或 `Plugins/GameCore/*.dll` | **自己编译** | 你的插件 DLL，导出 `OnLoad`（可选导出 `OnMessage`，无需实现 `OnUnload`） |
| `HookAPI.h` | Loader 发行包 | 插件工程唯一需要包含的 C/C++ 接口头文件 |

---

## 2. Mod 目录结构规范

### 先记住这三条

写插件前先确认这三件事，它们是最常见的失败原因：

1. **插件 DLL 必须放进子目录**：`Plugins/Global/` 或 `Plugins/GameCore/`。直接放在 `Plugins/` 根目录下的 DLL 会被扫描器忽略，不会加载。
2. **工程必须定义类别宏**：`CIV6_PLUGIN_GLOBAL` 或 `CIV6_PLUGIN_GAMECORE` 二选一。不定义则 `HookAPI.h` 直接 `#error` 拒绝编译。
3. **钩子与补丁互斥**：同一个地址不可既 `InstallHook` 又 `Patch`，两个方向都会被拒绝。要改一个已挂钩的函数，请在 detour 里做（详见 §6.3）。

### 目录模板

每个 Mod 的物理目录必须按规范组织：

```text
MyAwesomeMod/
  MyAwesomeMod.modinfo          ← UTF-8 无 BOM
  Data/
    Config.sql                  ← 用于首次引导接管，PackageId 填写本 modinfo 的 id
  Binaries/Win64/
    GameCore_XP2_Shell_FinalRelease.dll
    Civ6DllLoader.runtime.dll
    Plugins/
      Global/                   ← 全局常驻插件目录（按需选择）
        MyGlobalPlugin.dll
      GameCore/                 ← 对局生命周期插件目录（按需选择）
        MyGameplayPlugin.dll
```

> **重要扫描规则**：Loader 严格按子目录扫描插件：
> - `Binaries/Win64/Plugins/Global/*.dll`：识别为 **Global 插件**。
> - `Binaries/Win64/Plugins/GameCore/*.dll`：识别为 **GameCore 插件**。
> - **切勿直接把插件 DLL 放在 `Plugins/` 根目录下**，直接放在根目录的 DLL 会被扫描器忽略，无法加载。

### Config.sql 模板（引导配置）

```sql
UPDATE GameCores
SET
    PackageId = '你的-modinfo-GUID',
    DllPrefix = 'GameCore_XP2_Shell'
WHERE
    GameCore = 'Expansion2';
```
> 目前 Loader 核心功能针对 **风云变幻（Gathering Storm / EXPANSION_2）** 规则集。

---

## 3. 插件分类与生命周期模型

Loader 明确区分两类插件，两者具有截然不同的生命周期与调用权限：

| 插件类别 | 存放目录 | 预编译宏 | 生命周期 | 适用场景 |
|---------|---------|---------|---------|---------|
| **Global 插件** | `Plugins/Global/` | `CIV6_PLUGIN_GLOBAL` | 游戏启动早期（Phase A）加载，**进程内常驻，永不卸载** | 钩取 exe 域逻辑、常驻 UI 系统、拦截 Windows API、全局通用工具等 |
| **GameCore 插件** | `Plugins/GameCore/` | `CIV6_PLUGIN_GAMECORE` | 每次进入对局（Phase B）加载，**退出对局/返回主菜单/重载对局时卸载** | 修改游戏核心玩法规则、扩展 GameCore Lua 对象、挂钩 GameCore 内部逻辑等 |

### 插件导出函数规范

```cpp
#include "HookAPI.h"

// 必须导出：插件初始化入口
extern "C" __declspec(dllexport) void OnLoad(HookAPI* api);

// 可选导出：跨插件消息接收
extern "C" __declspec(dllexport) void OnMessage(const char* topic, const void* data, size_t len);

// 可选导出：历史遗留（通常不需要实现，不导出或保持空实现即可）
extern "C" __declspec(dllexport) void OnUnload();
```

### 生命周期规则与 OnUnload

1. **`OnLoad` 的幂等性要求**：
   - 在 `OnLoad` 开头必须首先调用 `HookAPI_IsCompatible(api)` 进行 ABI 版本防御。
   - Global 插件常驻进程，但其 `OnLoad` 仍应保证幂等性，不得假定其一生仅执行一次。
2. **`OnUnload`（历史遗留，无需实现）**：
   - Loader 已经全自动接管了所有框架级资源的清理工作：卸载插件时会自动拔除 MinHook 钩子、撤销参数捕获订阅、注销消息总线与回调监听。
   - 当退回主菜单或重载对局时，整个 GameCore 模块及其内存空间会被游戏引擎完全释放回收，下一局会重新加载全新的官方映像。
   - 因此，**插件作者通常完全不需要实现 `OnUnload`（直接不导出或保持空实现即可）**。切勿在 `OnUnload` 或 `DllMain(DLL_PROCESS_DETACH)` 里做复杂的内存析构，以免与宿主引擎的卸载顺序发生冲突。

---

## 4. 插件工程配置与编译要求

- **构建平台**：`x64`，配置建议使用 `Release`。
- **运行库**：推荐使用静态链接 C/C++ 运行时库（`Multi-threaded /MT`），避免目标机器缺少 VC 运行库 DLL。
- **头文件依赖**：工程中只需包含 `HookAPI.h`。
- **强制预处理宏定义（重要！）**：
  在工程属性「C/C++ -> 预处理器 -> 预处理器定义」中，**必须**显式声明以下两个宏之一（二选一）：
  - Global 插件添加：`CIV6_PLUGIN_GLOBAL`
  - GameCore 插件添加：`CIV6_PLUGIN_GAMECORE`

> **类别检查机制**：
> 1. `HookAPI.h` 根据该宏在编译期自动生成 `Civ6PluginKind` 导出函数（返回类别 1 或 2，以及 API 版本）。
> 2. Loader 在调用 `OnLoad` 之前，会读取该导出并核对插件所处的物理目录（`Global` 或 `GameCore`）。若宏声明与所在目录不一致，Loader 会在执行任何插件代码前**直接拒载**。
> 3. 若未定义宏，包含 `HookAPI.h` 将直接触发 `#error` 阻止编译。
> 4. 若为 Global 插件（定义了 `CIV6_PLUGIN_GLOBAL`），GameCore 专属接口（如 `RegisterScriptGlobals`、`PushLuaMethods`、`EditPlayer`、`GetEngineUtility`、`RegisterGameEvent` 等）将在编译期被宏重命名拦截，调用时直接报编译错误，防止跨生命周期非法调用。

---

## 5. 地址定位与跨版本兼容（重点）

> 本章是本文篇幅最大的一章。如果你是第一次写插件，可以先跳到第 6 章把最小可运行插件跑通，再回来读本章——但只要你的插件要挂钩游戏函数或读写内部对象，这些内容就是绕不开的。

文明 6 每次更新（小补丁、启动器变动、平台更新等）都会让 PE 二进制的代码段（`.text`）与数据段（`.data`）排布发生位移。直接写死绝对 RVA，或者按硬编码偏移逐层解引用结构体，都会在更新后立刻失效甚至崩溃。

Loader 内置了基于 PE 展开表（`.pdata`）的跨版本对齐机制（`PdataMatch`），符号库覆盖 **ExeDx11、ExeDx12 与 GameCoreXP2**。`HookAPI.h` 在此基础上提供了一组寻址封装，让插件不必自己处理版本差异。

本章按这个顺序读最省事：**§5.2 `HookInfo`** 是核心写法，直接用它就能覆盖绝大多数需求（§5.1 的 AOB 语法是它的兜底手段，遇到多命中时再回来查）。§5.3–§5.6 针对特定目标类型——exe 双版本、全局数据槽、虚表、无法用符号定位的内部对象。**§5.7 是可选的可用性判断**，不读也不影响写插件。

### 5.1 AOB 特征码语法

AOB（Array of Bytes）是本文多处用到的基础定位手段，语法由 `ResolveByPattern` 定义，`HookInfo.aob`、`ResolveDataByAob` 全部共用同一套规则。

**基本写法**：每个字节写成两位十六进制，字节之间可以用空格、Tab、逗号分隔，也可以直接连写（`488B??` 等价于 `48 8B ??`）。

| 写法 | 含义 | 示例 |
|------|------|------|
| `48` | 该字节必须精确等于 `0x48` | `48 89 5C 24` |
| `??` | 该字节任意取值（整字节通配） | `48 89 ?? ??` |
| `4?` | 高半字节固定为 `4`，低半字节任意 | 匹配 `0x40`–`0x4F` |
| `?F` | 低半字节固定为 `F`，高半字节任意 | 匹配 `0x0F`、`0x1F`…`0xFF` |

十六进制字母大小写均可。**通配符只能出现在半字节位置**，`?` 必须成对或与一个十六进制字符搭配，写出 `4` 或 `?5?` 这类奇数长度会直接解析失败并返回 `nullptr`。

```cpp
// 在 .text 段搜索首个命中（section 可换 kSectionImage / kSectionRdata 等）：
void* p = ResolveByPattern<void*>(api, kHookImageGameCoreXP2,
    "48 8B ?? ?? 74 ?? E8", kSectionText);

// 命中多处时，用 index 指定取升序第几个（从 1 起，默认 1）：
void* p2 = ResolveByPattern<void*>(api, kHookImageGameCoreXP2,
    "48 83 EC 28 8B 15 ?? ?? ?? ??", kSectionText, 2);
```

**两点使用注意**：

1. **AOB 打得越短越容易撞车**。默认只取**首个**命中，一条过短的特征串很可能在别处先命中，从而定位到完全无关的代码。建议直接把你实际看到的那段指令（通常 15–30 字节）整段抄下来，只把确实会随版本变化的位移和立即数部位通配掉。
2. **通配掉的范围决定了它的抗更新能力**。若把 `E8 ?? ?? ?? ??` 里的调用目标通配、却把紧随其后的 `48 89 87 30 02 00 00`（结构体偏移）写死，那么一旦结构体布局变化，这条 AOB 就会失效——**该通配的要通配到位**。

当一段特征码确实无法扩到唯一时（例如多个函数体逐字节相同，差异只在被通配的 `disp32` 上），就用 `index` 按升序命中表的位置消歧。同一 `index` 也可以直接写进 `HookInfo`：

```cpp
// 主路径用符号名；AOB 兜底时取升序第 2 个命中：
constexpr HookInfo kMyFunc = HookName("GameCore::Some::Func", "48 83 EC 28 8B 15 ?? ?? ?? ??", 2);
// 或配合 RVA 主路径：
constexpr HookInfo kMyFunc2 = HookRva(0x100F40, "48 83 EC 28 8B 15 ?? ?? ?? ??", 2);
```

> `index` 只在**同一段内**按地址升序计数。命中数不足 `index` 时返回 `nullptr` 并记日志（日志会打印实际命中数），不会越界读取。

**RIP 相对寻址专用语法**：用于 `ResolveDataByAob`（见 §5.4），在普通语法之上增加两条规则：

- 用 `|` 把特征串切成指令段，**含 `xx` 的那一段是目标指令**；
- `xx` 标记 4 字节 `disp32` 的位置。一次特征串内**必须恰好出现一组连续的 4 个 `xx`**，且必须落在同一段内，否则解析失败。

```cpp
// mov rcx, [rip+disp32]  |  call ...  |  mov [rdi+230h], rax
constexpr char kAob_UIManager[] = "48 8B 0D xx xx xx xx | E8 ?? ?? ?? ?? 48 89 87 30 02 00 00";
```

运行时按 `Hit + 目标段末偏移 + disp32` 算出绝对地址（即 `mov rcx, [rip+disp32]` 读的那个全局槽）。

### 5.2 统一多级寻址模型：`HookInfo`（主路径 + AOB 自动兜底）

推荐使用 `HookInfo` 结构体来定义你的 Hook 目标。它将「主路径（RVA/符号名/虚表）」与「备用路径（AOB 特征码兜底）」统一打包：

```cpp
// 方式 A：以 RVA 为主路径，附带 AOB 兜底
constexpr HookInfo kMyFunc = HookRva(0x64B400, "48 89 5C 24 ?? 57 48 83 EC 20");

// 方式 B：以符号名为主路径，附带 AOB 兜底
constexpr HookInfo kMyFuncByName = HookName(
    "GameCore::Quests::Manager::AttemptNewQuests",
    "48 89 5C 24 ?? 57 48 83 EC 20"
);

// 解析调用：
void* target = ResolveByHookInfo(api, kHookImageGameCoreXP2, kMyFunc);
```

**机制说明**：

1. **优先走 PdataMatch**：Loader 先拿旧 RVA 或符号名，用内嵌的展开表签名、序言哈希与单调链做跨版本对齐，找回当前地址。
2. **失败后降级**：若对齐无法确认，`ResolveByHookInfo` 自动回退到 AOB 特征扫描（底层同为 `ResolveByPattern`，沿用 `HookInfo.index`）。

### 5.3 Exe 域双版本适配：`HookInfoExe`

文明 6 有 DX11（`CivilizationVI.exe`）和 DX12（`CivilizationVI_DX12.exe`）两个独立构建，RVA 与指令排布各不相同。用 `HookInfoExe` 可以让 Global 插件同时适配两者：

```cpp
// 为 DX11 与 DX12 分别配置对应的 RVA 与 AOB：
constexpr HookInfoExe kExeLoadDLL = {
    HookRva(0xF8510, "48 81 EC 48 02 00 00 48 83 3D ?? ?? ?? ?? 00"),  // DX11 配置
    HookRva(0xF8850, "48 81 EC 48 02 00 00 48 83 3D ?? ?? ?? ?? 00")   // DX12 配置
};

// 自动按当前进程（DX11 或 DX12）解析对应配置：
void* target = ResolveByHookInfoExe(api, kExeLoadDLL);
```
`ResolveByHookInfoExe` 会探测当前进程的 exe 映像，选用匹配的那个分支，插件不必自己写判断。

### 5.4 全局数据与单例槽定位：`ResolveDataByAob`（RIP 相对寻址）

不要直接抄 `.data` 段的绝对 RVA——`.data` 段对变量增删极其敏感，一经更新该地址必然失效。

`.text` 段访问全局变量通常走 RIP 相对寻址（如 `mov rcx, [rip + disp32]`）。`ResolveDataByAob` 从这类指令里解出当前的真实数据槽：

```cpp
// 语法见 5.1：用 | 分割指令段，xx 标记 4 字节 disp32 偏移位置
constexpr char kAob_UIManager[] = "48 8B 0D xx xx xx xx | E8 ?? ?? ?? ?? 48 89 87 30 02 00 00";

// 解析得到的是该全局指针所在的内存槽地址（void**）
void* slot = ResolveDataByAob(api, kHookImageExeDx11, kAob_UIManager);
if (slot) {
    void* pUIManager = *reinterpret_cast<void**>(slot);
}
```
它读的是指令里的相对偏移，因此不受 `.data` 段基址或排布变动影响。

### 5.5 虚函数槽位符号寻址：`ResolveByVTable` / `HookVTable`

如果直接对虚函数指针硬编码偏移（如 `vtable[3]`），虚表结构若发生微调也会失效。`ResolveByVTable` 结合符号库按槽位序号解析：

```cpp
// 按虚表符号名与槽位序号（0 起始）解析：
void* pVFunc = api->ResolveByVTable(
    kHookImageGameCoreXP2,
    "GameCore::Quests::Manager::`vftable'",
    2  // 槽序号
);

// 也可以直接包装进 HookInfo：
constexpr HookInfo kMyVTableHook = HookVTable(
    "GameCore::Quests::Manager::`vftable'",
    2,
    "48 83 EC 28 ..."  // 可选的 AOB 兜底
);
```

### 5.6 获取内部全局对象：`RegisterGlobalCapture`

**用途**：拿那些没有导出符号、也无法用 `ResolveBy*` 定位的全局对象／单例（管理器、上下文、世界状态等）。`ResolveByRva` / `ResolveByName` 只能给出函数地址或已知符号的全局槽，而游戏里不少关键对象只以裸指针形式存在——`RegisterGlobalCapture` 就是从函数参数里截获它们的手段。

不要用 `*(void**)((char*)ptr + 0x1A8)` 这类硬编码偏移逐层解引用：结构体一旦调整，偏移全部错位，且往往是静默破坏内存。

正确做法是找到某个会接收该对象作为参数的函数，用 `RegisterGlobalCapture` 拦截寄存器传参（仅前 4 个寄存器参数 `rcx`, `rdx`, `r8`, `r9`，对应 paramIndex 0~3）：

```cpp
static void* g_pCapturedManager = nullptr;

// 找到一个会接收该 Manager 作为参数的函数入口（例如 void Init(..., Manager* mgr)）：
void* providerFunc = ResolveByName<void*>(api, kHookImageGameCoreXP2, "GameCore::SomeInitFunction");
if (providerFunc) {
    // 捕获第 2 个参数 (rdx，paramIndex=1) 并写入 g_pCapturedManager
    api->RegisterGlobalCapture(providerFunc, 1, &g_pCapturedManager);
}
```
Loader 用动态汇编（JIT Stub）在该函数入口截获该参数，写入你的指针槽。只要该函数的传参语义不变，即使对象内部结构如何变化，拿到的仍是正确指针。

> **与 `ResolveDataByAob`（§5.4）的分工**：后者解的是代码里 `mov reg, [rip+disp32]` 直接引用的全局槽，适合有固定指令引用它的单例；本节靠函数传参捕获，适合只在初始化时被传递、代码里没有固定引用点的对象。两者都不依赖结构体偏移。

### 5.7 可用性判断（可选）：`GetSymbolDbState`

前面几节的写法已经能自动处理版本差异——`HookInfo` 主路径失败会回退 AOB，`ResolveByName` / `ResolveByRva` 也会经 `PdataMatch` 矫正。**绝大多数插件不需要关心本节的内容**，照 §5.2 写就行。

本节介绍的两个接口提供的是一份**状态快照**：告诉你内置符号库与当前映像对得上多少。它在两类场景下有用：

- **需要区分"没找到"的原因**。解析返回 `nullptr` 时，到底是"这个函数在这个版本里真不存在"，还是"符号库根本不可用"？前者你应该关掉这个功能，后者提示用户更新 Loader 更合适。没有这个接口时日志里只能看到 `no symbol`。
- **功能对失效很敏感，宁可不开**。例如某个 hook 一旦定位错误就会破坏存档，而 AOB 兜底又有撞车风险——这时你可以要求符号库必须是 `Identical`，否则整个功能不启用。

```cpp
// 只在需要上述判断时才调；正常解析不需要它。
const HookApiSymbolDbState st = api->GetSymbolDbState(kHookImageGameCoreXP2);

void* fn = ResolveByHookInfo(api, kHookImageGameCoreXP2, kMyFunc);   // 主路径照旧
if (!fn) {
    if (st == kHookApiDbNoContext || st == kHookApiDbBuildFailed) {
        // 符号库整体不可用：不是"函数不存在"，而是"没法查"
        api->Log("symbol db unavailable (%d); ask user to update loader", static_cast<int>(st));
    } else {
        api->Log("target not found in this game version; feature disabled");
    }
}
```

各状态含义：

| 状态 | 含义 |
|---|---|
| `kHookApiDbIdentical` | 符号库与当前映像**同一构建**，RVA 恒等 |
| `kHookApiDbCrossVersion` | 跨版本对齐成功，旧 RVA 会被矫正 |
| `kHookApiDbNotReady` | 有该映像的符号数据，但尚未建表（时机偏早，稍后重查） |
| `kHookApiDbBuildFailed` | 建表失败，本次会话不做矫正 |
| `kHookApiDbNoContext` | sidecar 里根本没有该映像的模块 |

**`kHookApiDbBuildFailed` / `kHookApiDbNotReady`**（有该映像的符号数据，但表没建成）：`ResolveByRva` 与 `ResolveByName` 一律返回 `nullptr`，fail-closed，不会拿旧 RVA 去猜。

**`kHookApiDbNoContext`**（sidecar 里压根没有这个映像）：`ResolveByName` 返回 `nullptr`（没有符号可查）；但 `ResolveByRva` 会**直接把你给的 RVA 当成当前坐标去解析**——因为 Loader 没有依据做校正，只能原样透传。这既可能碰巧正确，也可能落到错误的函数上。看到这个状态时不要继续用旧 RVA，改用 `ResolveByPattern` 走 AOB，或者关闭该功能。

> 这两种"不可用"都不意味着该退回去用 `ResolveByRvaEx` 之类的裸 RVA 接口——符号库不可用时，旧 RVA 本来就没有任何依据。

**`GetSymbolDbMatchRatio(image)`** 返回跨版本对齐的匹配率（已对齐的 `.pdata` 条目 ÷ 总数），仅在 `kHookApiDbCrossVersion` 下是"部分匹配"的度量；`Identical` 返回 `1.0`，其余返回 `0.0`。适合写进日志帮助远程排查。

> 这两个接口只给判断，不改变解析行为——它们不能替代 `HookInfo` 的多途径定位。**单点能否解析，始终以 `ResolveBy*` 的返回值为准**：整体匹配率再高也可能恰好漏掉你要的那一个函数。
>
> `image` 取 `kHookImageExeDx11` / `kHookImageExeDx12` / `kHookImageGameCoreXP2`，各映像状态互相独立（例如 exe 可能 `Identical` 而 GameCore 是 `CrossVersion`）。

---

## 6. HookAPI 核心功能与使用示例

### 6.1 基础入口与兼容性防御

```cpp
extern "C" __declspec(dllexport) void OnLoad(HookAPI* api) {
    // 必须首先检查 ABI 兼容性
    if (!HookAPI_IsCompatible(api)) {
        if (api) api->Log("MyPlugin: Civ6DllLoader version is incompatible!");
        return;
    }

    api->Log("MyPlugin: OnLoad successfully initialized.");
}
```

### 6.2 安装函数 Hook（MinHook）

```cpp
using AttemptNewQuestsFn = void(__fastcall*)(void* thisPtr);
static AttemptNewQuestsFn g_origAttemptNewQuests = nullptr;

void __fastcall MyAttemptNewQuests(void* thisPtr) {
    // 执行前置逻辑
    g_origAttemptNewQuests(thisPtr);
    // 执行后置逻辑
}

// 在 OnLoad 中安装：
void* target = ResolveByName<void*>(api, kHookImageGameCoreXP2, "GameCore::Quests::Manager::AttemptNewQuests");
if (target) {
    InstallHook(api, target, reinterpret_cast<void*>(&MyAttemptNewQuests), &g_origAttemptNewQuests);
}
```

### 6.3 字节级内存补丁（Patch / Unpatch）

`Patch` 把硬编码机器码写进游戏内存（例如把条件跳转 nop 掉、改掉一个立即数），用于那些**不适合装钩子**的场景：

```cpp
static uint8_t s_nopBytes[] = { 0x90, 0x90 };
static void* s_patchTarget = nullptr;

// 在 OnLoad 中应用补丁：
s_patchTarget = ResolveByHookInfo(api, kHookImageGameCoreXP2,
    HookName("GameCore::Some::Func", "48 83 EC 28 ?? ?? ?? ??"));
if (s_patchTarget) {
    api->Patch(s_patchTarget, s_nopBytes, sizeof(s_nopBytes));
}
```

**使用约定**：

1. **同一地址同一插件只能打一次。** 重复 `Patch` 同一个 target 会被直接拒绝——补丁已生效，再叠一层就无法追踪哪层是谁的。要改字节，先 `Unpatch` 再 `Patch`。
2. **已被挂钩的点位不能打补丁。** Loader 双向禁止这种组合（MinHook 会改写目标前导字节，两边各存一份"原始字节"必然互相覆盖）：目标 32 字节前导窗口内有钩子 → `Patch` 拒绝；反过来，已有补丁的区间 → `InstallHook` 拒绝。要改一个已挂钩的函数，请在 detour 里实现。
3. **不同插件可以叠加补丁。** 每个 target 维护一个按应用顺序的 owner 栈，每层存的是"自己写入之前"的字节。因此 `Unpatch` 还原的是你打补丁时看到的状态，不会踩掉别人后打的补丁。
4. **按"生效后不还原"来设计。** 把补丁当作在进程生命周期内持续有效，不要在卸载或换局时去撤销它。Loader 本身也不做任何自动回滚。
5. **能装钩子就别打补丁。** 字节补丁与你抄下来的那个游戏版本绑定，更新后该处指令一变，写进去的就是垃圾；`InstallHook` 走 `PdataMatch` + AOB 双重定位，能跟随更新。

**`Unpatch` 慎用。** 它的正当场景是运行期动态开关某处修改（例如插件自带的配置项要即时生效）。用之前注意：

- **只能撤栈顶那一层**：若别的插件在你之后往同一地址打了补丁，你的 `Unpatch` 会被拒绝，得等它先撤。越过上层写回旧字节会静默覆盖它的补丁，所以 Loader 不允许这么做；
- **只有原打补丁的插件能还原**，且必须在它**仍然存活**时调用才可能成功。插件一旦因崩溃被 Loader 隔离（此后不再执行该插件的任何代码）或模块已卸载，就再无机会；
- 还原是在往**可能正被执行**的代码上写内存，没有 quiesce 兜底，因此不要把它放进卸载路径当作常规清理。

**结论**：`Patch` 是修改指令本身的正常手段；`Unpatch` 留给确实需要动态还原的场景，别当成卸载清理的常规步骤。

### 6.4 全局主 Lua VM 获取与 Lua 栈操作

Loader 提供了对引擎进程级主 Lua VM（`m_pkMainLuaState`）的直接访问能力，**Global 与 GameCore 插件均可安全调用**（仅限在游戏主线程中操作）：

```cpp
void* L = api->GetMainLuaState();
if (L) {
    api->LuaDoString(L, "print('Hello from C++ plugin via Main Lua State!')");
}
```

此外还有完整的 Lua 栈与表操作封装（类型校验、压栈、取值、表读写、`pcall` 等），与标准 Lua 5.1 C API 同名同义。完整清单见附录 §A.4。

### 6.5 扩展 GameCore Lua 对象方法（PushLuaMethods，GameCore 专属）

向游戏引擎已有的 C++ Lua 导出对象（如 `IQuestsManager`）挂载自定义方法：

```cpp
static int __fastcall lCustomMethod(void* L) {
    void* pMgr = api->LuaToUserData(L, 1);
    api->LuaPushInteger(L, 42);
    return 1;
}

static const LuaMethodInfo s_methods[] = {
    { lCustomMethod, "lCustomMethod", "CustomMethod" }
};

// 在 OnLoad 中挂载：
api->PushLuaMethods("GameCore::Lua::IQuestsManager", s_methods, 1);
```
之后在 Lua 脚本中即可直接调用：`Players[0]:GetQuestsManager():CustomMethod()`。

### 6.6 注册脚本全局环境（RegisterScriptGlobals，GameCore 专属）

在 gameplay 或 UI 的 Lua 全局表 `_G` 中注入自定义模块：

```cpp
static int __fastcall lDoSomething(void* L) {
    api->Log("DoSomething called from Lua!");
    return 0;
}

static void InitGlobals(void* L) {
    api->LuaNewGlobalTable(L, "MyMod", 0, 2);
    api->LuaPushCFunctionField(L, -1, "DoSomething", reinterpret_cast<void*>(&lDoSomething), "lDoSomething");
    api->LuaPop(L, 1);
}

// 注册到 gameplay Lua 初始流程：
api->RegisterScriptGlobals(&InitGlobals);
// 注册到 UI/Cache Lua 初始流程：
api->RegisterScriptGlobalsForUI(&InitGlobals);
```

### 6.7 游戏自定义事件总线（GameEvent）

支持在 C++ 端向 Lua 触发带任意参数的自定义 GameEvent（参数打包通过 `GameEventPushFn` 回调完成，Loader 负责加解引擎锁与异常防护）：

```cpp
// 1. 定义参数打包回调：向栈压入任意参数，返回参数个数
static int PushCustomEventArgs(void* L, const void* payload) {
    const int val = *reinterpret_cast<const int*>(payload);
    api->LuaPushInteger(L, val);
    api->LuaPushInteger(L, 100);
    return 2; // 向 Lua 传递了 2 个参数
}

// 2. 注册自定义事件名
api->RegisterGameEvent("MyModCustomEvent", &PushCustomEventArgs);

// 3. 在需要触发的地方调用（例如在 MinHook detour 中）：
int myPayload = 42;
api->TriggerGameEvent("MyModCustomEvent", &myPayload);
```
在游戏 Lua 端即可正常监听：
```lua
GameEvents.MyModCustomEvent.Add(function(param1, param2)
    print("Received custom event:", param1, param2)
end)
```

### 6.8 本地化与工具接口

- **本地化查询**：
  ```cpp
  void* loc = api->LocCreate();
  api->LocLookup(loc, "LOC_TECH_POTTERY_NAME");
  const char* text = api->LocToUtf8(loc);
  api->Log("Tech: %s", text ? text : "");
  api->LocDestroy(loc);
  ```
- **引擎一致的字符串哈希**：
  ```cpp
  uint32_t hash = api->MakeHash("YIELD_GOLD"); // 与 GameCore 内部 CRC32 严格一致，随时可用
  ```
- **获取玩家指针**：
  ```cpp
  void* pPlayer = api->EditPlayer(0); // 获取 0 号玩家指针（GameCore 专属）
  ```

---

## 7. 常见问题排查（FAQ）

### Q：玩家需要单独订阅或下载前置 Loader 吗？
**A：不需要。** 每个采用本体系的 Mod 都按规范在 `Binaries/Win64/` 自带薄壳与 `Civ6DllLoader.runtime.dll`。若玩家首次安装 Mod，薄壳会自动完成 `version.dll` 接管引导；若已存在 `version.dll`，则始终进入稳态运行。

### Q：多个不同 Mod 都带了 `Civ6DllLoader.runtime.dll`，会发生版本冲突吗？
**A：不会。** Loader 的竞选机制会自动比对所有已启用 Mod 中的 runtime，**始终选择版本最高的 runtime.dll 进行加载**。因此较新发布的 Mod 可以无缝带动老 Mod 享用更新的 Loader 功能与修复。

### Q：我的插件放到 `Binaries/Win64/Plugins/` 下为什么没有加载？
**A：必须放入子目录！** Loader 严格只扫描 `Plugins/Global/*.dll` 和 `Plugins/GameCore/*.dll`。请检查你的插件是否直接散落在了 `Plugins/` 根目录下。

### Q：编译报错 `#error "CIV6: 必须声明插件类别"` 怎么解决？
**A：必须在工程预处理器中定义类别宏。** 在工程属性「C/C++ -> 预处理器 -> 预处理器定义」中添加 `CIV6_PLUGIN_GLOBAL`（全局插件）或 `CIV6_PLUGIN_GAMECORE`（对局插件）。

### Q：为什么调用 `RegisterScriptGlobals` 或 `EditPlayer` 编译报错提示不是 HookAPI 成员？
**A：触发了编译期门禁保护。** 你的插件定义了 `CIV6_PLUGIN_GLOBAL`，而这些接口属于 GameCore 对局专属接口（生命周期受对局创建/销毁限制，常驻插件调用会发生空指针或非法访问）。如果确实是玩法逻辑插件，应转为 GameCore 插件（定义 `CIV6_PLUGIN_GAMECORE` 并放入 `Plugins/GameCore/` 目录）。

### Q：游戏更新后插件需要重新编译发布吗？
**A：通常不需要。** Loader 内置跨版本对齐（`PdataMatch`），插件若采用第 5 节推荐的 `HookInfo`、`ResolveByHookInfo` 或 `ResolveByName` 写法，旧 RVA 或符号名在更新后会自动矫正到新地址。若定位失败，可查看 `Documents/My Games/Sid Meier's Civilization VI/Logs/Civ6DllLoader_Runtime.log` 中的警告。

### Q：`OnUnload` 里要做什么清理吗？
**A：不需要，保持空实现或直接不导出即可。** Loader 会自动回收钩子、参数捕获与消息订阅；退局或重载时，整个 GameCore 模块由引擎完整释放。详见 §3 的生命周期说明。

### Q：用 `Patch` 改了内存，卸载插件时会自动还原吗？
**A：不会。** Loader 不对补丁做自动回滚，写入的字节在进程退出前持续有效。因此补丁应按"生效后不还原"来设计。`Unpatch` 可用于运行期动态撤销，但不要把它当作卸载清理的常规步骤（详见 §6.3）。

---

## 附录 A：HookAPI 接口全表

`HookAPI` 是一个**只读**函数指针表（写在 `.rdata`，写入会当场崩溃）。以下按用途分组列出全部 76 个入口。

**通用约定**：
- 所有 `void* L` 参数都是 Lua 状态指针，**必须**是 `GetMainLuaState()` 的返回值或回调（`ScriptGlobalsFn` / `GameEventPushFn` / `LuaMethodFunc`）传入的那个 `L`；传 `nullptr` 会被拒绝并记日志。
- 所有 Lua 栈索引遵循标准 Lua 约定：正数为栈底起算，负数为栈顶起算。
- 标 ✱ 的接口属于 **GameCore 专属**，Global 插件调用会在**编译期**报错（见 §4）。

### A.1 映像查询与取址

| 接口 | 签名 | 说明 |
|------|------|------|
| `IsImageLoaded` | `bool(HookImageId)` | 该映像当前是否已加载。用于判断 GameCore 是否就绪。 |
| `GetImageInfo` | `bool(HookImageId, HookImageInfo*)` | 取映像基址与大小。调用前把 `outInfo->structSize` 设为 `sizeof(HookImageInfo)`。 |
| `ResolveByRva` | `void*(HookImageId, uintptr_t rva, size_t span)` | 按**旧版本 RVA** 解析，自动经 `PdataMatch` 矫正到当前版本。`span` 是要求可读的字节数，默认为 1。符号库无该映像时不做矫正、直接按原值解析（见 §5.7）。 |
| `ResolveByName` | `void*(HookImageId, const char*)` | 按符号名解析（demangled 友好名与 MSVC 修饰名都支持），同样经过跨版本矫正。 |
| `ResolveByRvaEx` | `void*(HookImageId, uintptr_t rva, size_t span, HookImageSection)` | **不做**矫正的裸 RVA，可指定段。仅在你已自行算好当前版本坐标时使用。 |
| `ResolveByPattern` | `void*(HookImageId, const char* pattern, HookImageSection, uint32_t index)` | AOB 特征搜索（语法见 §5.1），返回指定段内升序第 `index` 个命中（从 1 起，传 0 视为 1）。 |
| `ResolveByVTable` | `void*(HookImageId, const char* vtableSymbol, uint32_t index)` | 按虚表符号名 + 槽序号（0 起）解析虚函数地址。 |
| `GetSymbolDbState` | `HookApiSymbolDbState(HookImageId)` | 符号库与映像的匹配状态快照。可选，用于区分"函数不存在"与"符号库不可用"，见 §5.7。 |
| `GetSymbolDbMatchRatio` | `double(HookImageId)` | 跨版本对齐的匹配率 `[0.0, 1.0]`，适合写进日志排查，见 §5.7。 |

```cpp
// 典型的"带兜底"三步写法：
void* fn = ResolveByName<void*>(api, kHookImageGameCoreXP2, "GameCore::Foo::Bar");
if (!fn) fn = ResolveByPattern<void*>(api, kHookImageGameCoreXP2, "48 89 5C 24 ?? 57", kSectionText);
if (!fn) { api->Log("Foo::Bar not found; feature disabled"); return; }
```

> 更推荐的写法是用 `HookInfo` + `ResolveByHookInfo`，它把上面这套兜底逻辑内置了（见 §5.2）。

### A.2 钩子、补丁与变量捕获

| 接口 | 签名 | 说明 |
|------|------|------|
| `InstallHook` | `bool(void* target, void* detour, void** original)` | 安装 MinHook detour，原函数跳板写入 `*original`。插件卸载时 Loader 会自动摘钩。 |
| `Patch` | `bool(void* target, const void* bytes, size_t len)` | 原始字节写入，用于修改指令本身。同一 `target` 同插件只能打一次；已被挂钩的点位会被拒绝。见 §6.3。 |
| `Unpatch` | `bool(void* target)` | 还原你自己那一层补丁。**慎用**：只能撤栈顶那层，别当卸载清理用，见 §6.3。 |
| `RegisterGlobalCapture` | `bool(void* providerAddress, uint32_t paramIndex, void** out)` | 拦截 `providerAddress` 的第 `paramIndex` 个寄存器参数（0–3 对应 rcx/rdx/r8/r9）写入 `*out`。获取未导出全局对象的标准手段，见 §5.6。 |

```cpp
// InstallHook 的常用模板（注意 detour 必须与目标函数调用约定一致）：
using BarFn = int(__fastcall*)(void* self, int a);
static BarFn g_origBar = nullptr;

int __fastcall DetourBar(void* self, int a) {
    if (a < 0) a = 0;                 // 前置修正
    return g_origBar(self, a);        // 调用原函数
}

if (void* t = ResolveByName<void*>(api, kHookImageGameCoreXP2, "GameCore::Foo::Bar")) {
    InstallHook(api, t, reinterpret_cast<void*>(&DetourBar), &g_origBar);
}
```

```cpp
// 捕获一个无法通过符号拿到的单例：
static void* g_pWorld = nullptr;
if (void* provider = ResolveByName<void*>(api, kHookImageGameCoreXP2, "GameCore::World::Init")) {
    api->RegisterGlobalCapture(provider, 0, &g_pWorld);   // 取第 1 个参数 (rcx)
}
// 之后 g_pWorld 会被自动填充；使用前判空即可。
```

### A.3 消息总线（插件间通信）

| 接口 | 签名 | 说明 |
|------|------|------|
| `RegisterMessageHandler` | `bool(MessageHandlerFn)` | 注册消息回调。**地址固定为插件 DLL 文件名**（去扩展名，UTF-8 区分大小写），不接受自定义名。 |
| `BroadcastMessage` | `void(const char* topic, const void* data, size_t len)` | 向所有已注册插件广播。 |
| `SendMessageTo` | `void(const char* pluginName, const char* topic, const void* data, size_t len)` | 点对点发送，`pluginName` 为对方 DLL 主文件名。 |

若插件导出了 `OnMessage`，Loader 会在 `OnLoad` 返回后自动以插件文件名完成注册，**通常无需手写** `RegisterMessageHandler`。

```cpp
extern "C" __declspec(dllexport)
void OnMessage(const char* topic, const void* data, size_t len) {
    if (strcmp(topic, "config-changed") == 0) { /* ... */ }
}

// 主动发消息：
api->BroadcastMessage("config-changed", nullptr, 0);
api->SendMessageTo("OtherPlugin", "ping", "hi", 2);
```

### A.4 Lua 状态与栈操作

以下接口直接映射游戏引擎内嵌的 Lua 虚拟机 HavokScript 导出，语义与标准 Lua 5.1 C API 一致。任一底层导入缺失时，接口会记日志并返回安全默认值（0 / nullptr），**不会崩溃**。

| 分类 | 接口 |
|------|------|
| 获取状态 | `GetMainLuaState`（Global 与 GameCore 均可调用，仅限游戏线程，换局不失效） |
| 栈操作 | `LuaGetTop` `LuaPop` `LuaPushValue` `LuaPushNil` `LuaToCFunction` |
| 压栈 | `LuaPushInteger` `LuaPushNumber` `LuaPushBoolean` `LuaPushLightUserData` `LuaPushFString` |
| 取栈 | `LuaToInteger` `LuaToNumber` `LuaToBoolean` `LuaToUserData` `LuaType` `LuaIsNumber` `LuaIsUserData` `LuaObjLen` |
| 校验参数 | `LuaCheckInteger` `LuaCheckNumber` `LuaCheckLString` `LuaCheckTable` `LuaOptInteger` |
| 表操作 | `LuaCreateTable` `LuaGetField` `LuaSetField` `LuaGetTable` `LuaSetTable` `LuaRawGetI` |
| 引用 | `LuaRef` `LuaUnref` |
| 调用 | `LuaPcall` `LuaCPcall` `LuaDoString` |
| 调试 | `LuaOpenDebug` `LuaSetHook` `LuaGetHook` `LuaGetHookMask` `LuaGetHookCount` |
| 错误 | `LuaError`（抛出 Lua 错误，**不返回**） |
| Civ6 专用 | `LuaCheckPlayerId`（校验栈上是合法玩家 ID，非法返回 -1） |

```cpp
// LuaDoString：直接执行一段 Lua 代码
void* L = api->GetMainLuaState();
if (L) {
    if (api->LuaDoString(L, "print('hello from plugin')") != 0) {
        api->Log("LuaDoString failed");
    }
}
```

```cpp
// LuaCPcall：受保护的 C 调用，用于安全地调用插件自己的 C 函数
static int __fastcall MyCallback(void* L) {
    api->LuaPushInteger(L, 123);
    return 1;
}
const int rc = api->LuaCPcall(L, &MyCallback, nullptr);
// rc == 0 成功；rc > 0 = 引擎 Lua 错误状态；rc < 0 = 参数非法/导入缺失
```

### A.5 Lua 全局表与方法扩展

| 接口 | 签名 | 说明 |
|------|------|------|
| `LuaNewGlobalTable` | `void(void* L, const char* name, int narr, int nrec)` | 创建全局表并留在栈顶。`narr`/`nrec` 为数组/哈希预分配提示。 |
| `LuaPushCFunctionField` | `void(void* L, int tableIndex, const char* field, void* fn, const char* internalName)` | 往表里挂一个 C 函数字段。`internalName` 是引擎侧调试用的名字。 |
| `LuaPushNamedCClosure` | `void(void* L, void* fn, int nup, const char* name, int narr)` | 压入带名字的 C 闭包。 |
| `RegisterScriptGlobals` ✱ | `void(ScriptGlobalsFn)` | 注册 gameplay Lua 构建完成时的回调。 |
| `RegisterScriptGlobalsForUI` ✱ | `void(ScriptGlobalsFn)` | 同上，但用于 UI/Cache Lua。 |
| `PushLuaMethods` ✱ | `HookApiPushMethodsResult(const char* name, const LuaMethodInfo*, uint32_t count)` | 给已有 C++ Lua 对象挂方法，见 §6.5。 |
| `RegisterPushMethodsTarget` ✱ | `int(const char* name, int sigType, const char* pushMethodsSymbol)` | 登记新的 PushMethods 目标对象，见下。 |

> 前三项只操作 Lua 栈，**不限插件类别**；后四项涉及 GameCore 的 Lua 对象与环境，因此带类别门禁。

```cpp
// 全局表注入（§6.6 的完整版）：
static int __fastcall lAdd(void* L) {
    const int a = api->LuaCheckInteger(L, 1);
    const int b = api->LuaCheckInteger(L, 2);
    api->LuaPushInteger(L, a + b);
    return 1;
}

static void InitMyMod(void* L) {
    api->LuaNewGlobalTable(L, "MyMod", 0, 4);
    api->LuaPushCFunctionField(L, -1, "Add", reinterpret_cast<void*>(&lAdd), "lAdd");
    api->LuaPop(L, 1);
}

api->RegisterScriptGlobals(&InitMyMod);        // Lua: MyMod.Add(1, 2) -> 3
api->RegisterScriptGlobalsForUI(&InitMyMod);   // UI Lua 里同样可用
```

```cpp
// 给一个 Loader 尚未内置登记的 Lua 对象加方法：
api->RegisterPushMethodsTarget("GameCore::Lua::IMyManager", kHookApiPushNoManager, nullptr);
// sigType：kHookApiPushNoManager（无 manager 前缀）/ kHookApiPushWithManager（带 manager 签名）
// 第三个参数为 nullptr 时，Loader 用 name + "::PushMethods" 作为解析符号。
api->PushLuaMethods("GameCore::Lua::IMyManager", s_methods, count);
```

> `LuaMethodInfo` 必须是 **C 布局**且三个字段都是 `const char*` 字面量，**不要**跨 DLL 传 `std::string` / `std::vector`。

### A.6 游戏事件总线

| 接口 | 签名 | 说明 |
|------|------|------|
| `RegisterGameEvent` ✱ | `bool(const char* name, GameEventPushFn)` | 登记一个自定义 GameEvent。名称最长 95 字节，超长返回 false。同名重复注册幂等返回 true。 |
| `TriggerGameEvent` ✱ | `bool(const char* name, const void* payload)` | 触发一次。必须先注册；`payload` 同步传给 `pushFn`，仅本次调用期间有效。 |

```cpp
static int PushArgs(void* L, const void* payload) {
    const auto* p = static_cast<const MyStruct*>(payload);
    api->LuaPushInteger(L, p->a);      // 参数 1
    api->LuaPushBoolean(L, p->flag);   // 参数 2
    return 2;                          // 返回压入的参数个数
}

api->RegisterGameEvent("MyModEvent", &PushArgs);

MyStruct s{ 42, true };
api->TriggerGameEvent("MyModEvent", &s);
```
```lua
GameEvents.MyModEvent.Add(function(a, flag) print(a, flag) end)
```

> **必须在游戏线程调用。** Loader 会在内部处理引擎锁的取放；若无法判定锁状态，它会宁可拒绝触发也不在持锁状态下重入 Lua。

### A.7 本地化

| 接口 | 签名 | 说明 |
|------|------|------|
| `LocCreate` | `void*()` | 创建一个本地化字符串对象。用完必须 `LocDestroy`。 |
| `LocDestroy` | `void(void*)` | 销毁对象。 |
| `LocLookup` | `void*(void* loc, const char* key)` | 按 key 查询（如 `"LOC_TECH_POTTERY_NAME"`）。 |
| `LocPushArgument` | `int(void* loc, const char* arg)` | 追加格式化参数（对应 `{1_TEXT}` 等占位）。 |
| `LocToUtf8` | `const char*(void*)` | 取 UTF-8 结果字符串。 |
| `LocStringBytes` | `uint32_t()` | 返回栈缓冲所需字节数（当前 256），用于 `CIV6_HOOK_API_LOC_STRING_BYTES`。 |
| `LocalizationLookup` | `void*(void* outStr, const char* key)` | 免分配的查询：`outStr` 指向调用方自己的栈缓冲。 |

```cpp
// 方式一：免分配（性能更好，推荐在高频路径用）
char buf[CIV6_HOOK_API_LOC_STRING_BYTES];
if (api->LocalizationLookup(buf, "LOC_TECH_POTTERY_NAME")) {
    api->Log("Tech: %s", buf);
}

// 方式二：带参数格式化
void* loc = api->LocCreate();
api->LocLookup(loc, "LOC_UNIT_GAINED_EXPERIENCE");
api->LocPushArgument(loc, "Warrior");
const char* text = api->LocToUtf8(loc);
api->Log("Msg: %s", text ? text : "");
api->LocDestroy(loc);
```

### A.8 游戏状态与工具

| 接口 | 签名 | 说明 |
|------|------|------|
| `EditPlayer` ✱ | `void*(int playerId)` | 取玩家对象指针。对局开始前返回 nullptr。 |
| `GetEngineUtility` ✱ | `void*()` | 取 EngineUtility 全局对象（用于自行取放 GameCore 锁）。GameCore 未就绪返回 nullptr。 |
| `MakeHash` | `uint32_t(const char*)` | 与引擎 `GameCore::Utilities::MakeHash` 逐位一致的 CRC32。**纯本地计算**，对局前/重载后都能用。 |
| `Log` | `void(const char* fmt, ...)` | 写 `Civ6DllLoader_Runtime.log`。 |

```cpp
uint32_t h = api->MakeHash("QUEST_SEND_TRADE_ROUTE");   // 与引擎内部哈希一致
api->Log("hash of quest = 0x%08X", h);
```

```cpp
// 需要在一段 Lua 操作期间独占 GameCore 锁时（与官方 CallHook 同款三步）：
void* eu = api->GetEngineUtility();
if (eu) {
    auto* vt = *reinterpret_cast<void***>(eu);
    using Fn0 = bool(__fastcall*)(void*);
    using Fn1 = void(__fastcall*)(void*);
    const bool had = reinterpret_cast<Fn0>(vt[6])(eu);   // HasGameCoreLock
    if (had) reinterpret_cast<Fn1>(vt[7])(eu);           // ReleaseGameCoreLock
    // ... 此处做需要独占的 Lua 工作 ...
    if (had) reinterpret_cast<Fn1>(vt[8])(eu);           // GetGameCoreLock
}
```

> 绝大多数场景 **不需要**手动取锁——`TriggerGameEvent`、`PushLuaMethods` 等高层接口已在内部处理。仅在你直接调用引擎内部函数时才需要。

### A.9 兼容性检查

```cpp
static inline int HookAPI_IsCompatible(const HookAPI* api);
```

| 头文件内的模板工具 | 说明 |
|--------------------|------|
| `ResolveByName<T>(api, image, name)` | `ResolveByName` 的返回类型转换封装。 |
| `ResolveByRva<T>(api, image, rva, span)` | 同上。 |
| `ResolveByRvaEx<T>(api, image, rva, section, span)` | 同上。 |
| `ResolveByPattern<T>(api, image, pattern, section, index)` | 同上，`index` 默认为 1。 |
| `ResolveByVTable<T>(api, image, sym, index)` | 同上。 |
| `InstallHook(api, target, detour, &orig)` | 免去手动 `reinterpret_cast`。 |
| `HookRva` / `HookName` / `HookVTable` | 构造 `HookInfo` 的 `constexpr` 工厂。 |
| `ResolveByHookInfo(api, image, info, span)` | 解析 `HookInfo`（主路径失败自动走 AOB 兜底）。 |
| `ResolveByHookInfoExe(api, infoExe, span)` | 自动识别 DX11/DX12 后解析。 |
| `ResolveDataByAob(api, image, aob, index)` | RIP 相对寻址，解出全局数据槽。 |

---

## 附录 B：最小可用插件完整示例

一个 GameCore 插件：往 Lua 注入 `MyMod.Version()`，并在进入对局时打印一次日志。

**工程设置**：x64 / Release，预处理器定义 `CIV6_PLUGIN_GAMECORE`，包含 `HookAPI.h`。

```cpp
// MyGameplayPlugin.cpp
#include "HookAPI.h"
#include <string.h>

static HookAPI* g_api = nullptr;

static int __fastcall lVersion(void* L) {
    g_api->LuaPushFString(L, "MyMod v1.0");
    return 1;
}

static void OnInitGlobals(void* L) {
    g_api->LuaNewGlobalTable(L, "MyMod", 0, 2);
    g_api->LuaPushCFunctionField(L, -1, "Version", reinterpret_cast<void*>(&lVersion), "lVersion");
    g_api->LuaPop(L, 1);
}

extern "C" __declspec(dllexport) void OnLoad(HookAPI* api) {
    if (!HookAPI_IsCompatible(api)) {
        if (api) api->Log("MyMod: loader too old, please update");
        return;
    }
    g_api = api;
    api->Log("MyMod: loaded");

    api->RegisterScriptGlobals(&OnInitGlobals);
}

// 可选：接收其它插件的消息
extern "C" __declspec(dllexport)
void OnMessage(const char* topic, const void* data, size_t len) {
    g_api->Log("MyMod: got topic=%s len=%zu", topic ? topic : "?", len);
}
```

**部署**：把编译出的 `MyGameplayPlugin.dll` 放进 `MyMod/Binaries/Win64/Plugins/GameCore/`。

**验证**：进对局后打开 FireTuner 执行 `print(MyMod.Version())`，应输出 `MyMod v1.0`；同时 `Civ6DllLoader_Runtime.log` 中应出现 `MyMod: loaded`。
