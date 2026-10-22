# Civ6DllLoader 插件开发指南

> 中文 | [English](PLUGIN_GUIDE.en.md)

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
  → 主菜单建好后，把各 Mod 目录里的 FrontEndUI_*.xml 挂进主菜单（无需插件 DLL）

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
| `UI/FrontEnd/FrontEndUI_*.xml` + `.lua` | **自己写** | 主菜单界面，**不需要编译**；放进 Mod 目录即可，不必登记（见 §7） |

---

## 2. Mod 目录结构规范

### 2.0 先记住这四条

写插件前先确认这四件事，它们是最常见的失败原因：

1. **插件 DLL 必须放进子目录**：`Plugins/Global/` 或 `Plugins/GameCore/`。直接放在 `Plugins/` 根目录下的 DLL 会被扫描器忽略，不会加载。
2. **工程必须定义类别宏**：`CIV6_PLUGIN_GLOBAL` 或 `CIV6_PLUGIN_GAMECORE` 二选一。不定义则 `HookAPI.h` 直接 `#error` 拒绝编译。
3. **工程必须带版本资源**（`.rc` 里的 `VERSIONINFO`）。**没有文件版本的插件不会被加载**——见 §2.2。
4. **钩子与补丁互斥**：同一个地址不可既 `InstallHook` 又 `Patch`，两个方向都会被拒绝。要改一个已挂钩的函数，请在 detour 里做（详见 §6.3）。

### 2.1 目录模板

每个 Mod 的物理目录必须按规范组织：

```text
MyAwesomeMod/
  MyAwesomeMod.modinfo          ← UTF-8 无 BOM
  Data/
    Config.sql                  ← 用于首次引导接管，PackageId 填写本 modinfo 的 id
  UI/FrontEnd/                  ← 主菜单界面（按需，见 §7；不需要编译）
    FrontEndUI_MyMod.xml
    FrontEndUI_MyMod.lua
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

> 只有主菜单界面、且不需要 C++ 逻辑的 Mod，可以**完全不编译 DLL**——只保留薄壳 + runtime 加上
> `UI/FrontEnd/` 下的一对 `FrontEndUI_*.xml` / `.lua`（见 §7）。

### 2.2 版本资源（强制要求）

**插件 DLL 必须带 `VERSIONINFO` 文件版本，否则 Loader 不会加载它。**

这条不是建议，是加载前的门禁：扫描时读不到版本的 DLL 直接被拒绝，日志里留一行
`Rejected plugin (no VERSIONINFO file version resource): <路径>`。

#### 怎么加

在插件工程里加一个 `.rc` 文件（下面的内容可以直接抄），再把 `.vcxproj` 里加上它：

```xml
<ItemGroup>
  <ResourceCompile Include="Version.rc" />
</ItemGroup>
```

```rc
VS_VERSION_INFO VERSIONINFO
 FILEVERSION 1,0,0,0
BEGIN
END
```

就这三行。**整个文件只有一个版本号，改版本只改 `FILEVERSION` 那一行。**

#### Loader 怎么用它

**① 没有版本 → 不加载**（上面说的门禁）。

**② 同名插件只加载一份，留版本最高的。**

多个 Mod 各带一份同类插件时很常见（比如都基于同一个基础插件改的）。Loader 按
**文件名（大小写不敏感）** 分组，组内只留**文件版本最高**的那份：

```
ModA/Plugins/GameCore/Foo.dll   v1.0.0.0   ← 淘汰
ModB/Plugins/GameCore/Foo.dll   v1.2.0.0   ← 加载
ModC/Plugins/GameCore/Foo.dll   v1.1.0.0   ← 淘汰
```

日志里能看到裁决结果：

```
Duplicate plugin basename: kept <ModB 的路径> (v1.2.0.0), dropped <ModA 的路径> (v1.0.0.0, lower version)
```

**版本相同时**按路径字典序裁决（大小写不敏感），保证同一台机器每次启动结果一致：

```
Duplicate plugin basename: kept ... (v1.0.0.0), dropped ... (v1.0.0.0, same version -> lexicographic tie-break)
```

#### 几条要注意的

- **只有已启用 Mod 里的插件参与竞争**。禁用的 Mod 连候选都算不上；
- **`Global` 与 `GameCore` 各自独立分组**。`Plugins/Global/Foo.dll` 和
  `Plugins/GameCore/Foo.dll` 是两个不同的东西，各留各的；
- **不同文件名互不影响**。`Foo.dll` 和 `FooEx.dll` 是两组；
- **版本号是 `a.b.c.d` 四段比较**，`1.10.0.0 > 1.9.0.0`（按数值，不是按字符串）；
- **`0.0.0.0` 等同于没有版本**，会被拒绝。想参与选举就填一个非零版本。

> 如果你的插件是**独占**的（不希望被别的 Mod 带的高版本顶掉），把版本号抬到足够高，
> 或者干脆用一个别人不会用的文件名。

### 2.3 Config.sql 模板（引导配置）

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

// 必须导出：插件初始化入口。返回 0 = 初始化失败，非 0 = 成功
extern "C" __declspec(dllexport) int OnLoad(HookContext* ctx);

// 可选导出：跨插件消息接收
extern "C" __declspec(dllexport) void OnMessage(const char* topic, const void* data, size_t len);

// 可选导出：历史遗留（通常不需要实现，不导出或保持空实现即可）
extern "C" __declspec(dllexport) void OnUnload();
```

### 接口表的取得

`OnLoad` 拿到的是 `HookContext*`，不是接口表。用两行把它换成你能直接调的表：

```cpp
static HookAPI* g_api = nullptr;   // 跨函数使用就存起来；只用一次也可以不存

extern "C" __declspec(dllexport) int OnLoad(HookContext* ctx) {
    if (!HookContext_IsCompatible(ctx)) return 0;    // ABI 防御

    HookAPI* api = HookAPI_Resolve(ctx);             // 按名字解析出本模块的表
    if (!HookAPI_RequireAll(api, &HookAPI::Log,      // 声明自己真正依赖的入口
                            &HookAPI::ResolveByName)) {
        return 0;                                    // 缺项：Loader 版本不匹配，拒绝加载
    }
    g_api = api;

    // 之后照常调用
    api->Log("my plugin loaded");
    void* target = api->ResolveByName(kHookImageGameCoreXP2, "Some::Symbol");
    return 1;                                        // 初始化成功
}
```

要点：

- **`HookAPI_Resolve` 幂等**：内部是函数局部 `static`，首次调用把所有名字查一遍并填表，
  之后再调直接返回同一个指针。同一插件的多个 `.cpp` 共享这一份（`inline` 函数 + COMDAT 合并），
  所以每个文件里都可以放心调，不会各建一份表；
- **`HookAPI_RequireAll` 只列你自己用到的入口**。`HookAPI_Resolve` 会把所有格子填满，
  查不到的名字是 `nullptr`（绝不是野指针），但不检查就调用会空指针崩溃。
  `RequireAll` 让你在 `OnLoad` 里发现缺项并干净退出（`return 0`），而不是在某个回调里炸掉；
- 参数用成员指针（`&HookAPI::ResolveByName`）而不是字符串，所以名字拼错是**编译错误**、
  IDE 能补全、重命名能跟随；
- **`OnLoad` 返回 0 = 干净拒绝**：Loader 会回滚本次加载里已经登记的一切（钩子、捕获、
  脚本全局、消息），**不激活、不注册 `OnMessage`**，并把该插件标记为不可用（本次进程内不再重试）。
  所以"缺接口就退出"必须写成 `return 0`，不能只是 `return;`——后者在旧式 `void OnLoad` 里
  等于什么都没发生，插件仍会被激活。

### 生命周期规则与 OnUnload

1. **`OnLoad` 的幂等性要求**：
   - 在 `OnLoad` 开头必须首先调用 `HookContext_IsCompatible(ctx)` 进行 ABI 版本防御。
   - Global 插件常驻进程，但其 `OnLoad` 仍应保证幂等性，不得假定其一生仅执行一次。
2. **`OnUnload`（历史遗留，无需实现）**：
   - Loader 已经全自动接管了所有框架级资源的清理工作：卸载插件时会自动拔除 MinHook 钩子、撤销参数捕获订阅、注销消息总线与回调监听。
   - 当退回主菜单或重载对局时，整个 GameCore 模块及其内存空间会被游戏引擎完全释放回收，下一局会重新加载全新的官方映像。
   - 因此，**插件作者通常完全不需要实现 `OnUnload`（直接不导出或保持空实现即可）**。切勿在 `OnUnload` 或 `DllMain(DLL_PROCESS_DETACH)` 里做复杂的内存析构，以免与宿主引擎的卸载顺序发生冲突。

---

## 4. 插件工程配置与编译要求

- **构建平台**：`x64`，配置建议使用 `Release`。
- **运行库**：推荐使用静态链接 C/C++ 运行时库（`Multi-threaded /MT`），避免目标机器缺少 VC 运行库 DLL。
- **头文件依赖**：工程中只需包含 `HookAPI.h`（自包含，不依赖其它头文件）。
- **版本资源（必需）**：工程里要有一个 `Version.rc`，并在 `.vcxproj` 中加
  `<ResourceCompile Include="Version.rc" />`。**没有 `VERSIONINFO` 文件版本的 DLL 不会被加载**，
  同名插件之间也按这个版本号择优。模板与规则见 §2.2。
- **强制预处理宏定义（重要！）**：
  在工程属性「C/C++ -> 预处理器 -> 预处理器定义」中，**必须**显式声明以下两个宏之一（二选一）：
  - Global 插件添加：`CIV6_PLUGIN_GLOBAL`
  - GameCore 插件添加：`CIV6_PLUGIN_GAMECORE`

> **类别检查机制**：
> 1. `HookAPI.h` 根据该宏在编译期自动生成 `Civ6PluginKind` 导出函数（返回类别 1 或 2，以及 API 版本）。
> 2. Loader 在调用 `OnLoad` 之前，会读取该导出并核对插件所处的物理目录（`Global` 或 `GameCore`）。若宏声明与所在目录不一致，Loader 会在执行任何插件代码前**直接拒载**。
> 3. 若未定义宏，包含 `HookAPI.h` 将直接触发 `#error` 阻止编译。
> 4. Global 插件下，GameCore 专属接口（`GetPlayer`、`RegisterGameEvent`、`TriggerGameEvent`、`RegisterGameCoreScriptForGamePlay`、`RegisterGameCoreScriptForUI`、`PushLuaMethods`）**不会生成对应成员**，调用时报
>    `error C2039: "GetPlayer": 不是 "HookAPI" 的成员`，防止跨生命周期非法调用。
>
> **版本不匹配时的表现**：`HookContext_IsCompatible(ctx)` 返回 0。它比较 `structSize` 与 `apiVersion` —— 若插件与 Loader 不是同一次构建（改了头文件却只重编了一边），
> 两边对 `HookContext` 字段偏移的解释不同，该检查会在调用任何函数指针之前失败，因此不会出现错位调用。

---

## 5. 地址定位与跨版本兼容（重点）

> 本章是本文篇幅最大的一章。如果你是第一次写插件，可以先跳到第 6 章把最小可运行插件跑通，再回来读本章——但只要你的插件要挂钩游戏函数或读写内部对象，这些内容就是绕不开的。

文明 6 每次更新（小补丁、启动器变动、平台更新等）都会让 PE 二进制的代码段（`.text`）与数据段（`.data`）排布发生位移。直接写死绝对 RVA，或者按硬编码偏移逐层解引用结构体，都会在更新后立刻失效甚至崩溃。

Loader 内置了基于 PE 展开表（`.pdata`）的跨版本对齐机制（`PdataMatch`），符号库覆盖 **ExeDx11、ExeDx12 与 GameCoreXP2**。`HookAPI.h` 在此基础上提供了一组寻址封装，让插件不必自己处理版本差异。

本章按这个顺序读最省事：**§5.2 `HookInfo`** 是核心写法，直接用它就能覆盖绝大多数需求（§5.1 的 AOB 语法是它的兜底手段，遇到多命中时再回来查）。§5.3–§5.6 针对特定目标类型——exe 双版本、全局数据槽、虚表、无法用符号定位的内部对象。**§5.7 是可选的可用性判断**，不读也不影响写插件；**§5.8 是查符号名的工具**（手上只有 RVA 时先用它）。**§5.9 讲函数序言校验的适用范围**——如果你打算用它来确认目标函数，先读它再决定。

### 5.1 AOB 特征码语法

AOB（Array of Bytes）是本文多处用到的基础定位手段，语法由 `ResolveByPattern` 定义，`HookInfo.aob`、`ResolveDataByAob` 全部共用同一套规则。

**基本写法**：每个字节写成两位十六进制，字节之间可以用空格、Tab、逗号分隔，也可以直接连写（`488B??` 等价于 `48 8B ??`）。

| 写法 | 含义 | 示例 |
|------|------|------|
| `48` | 该字节必须精确等于 `0x48` | `48 89 5C 24` |
| `??` | 该字节任意取值（整字节通配） | `48 89 ?? ??` |
| `4?` | 高半字节固定为 `4`，低半字节任意 | 匹配 `0x40`–`0x4F` |
| `?F` | 低半字节固定为 `F`，高半字节任意 | 匹配 `0x0F`、`0x1F`…`0xFF`（低半字节为 `F` 的全部 16 个值） |

十六进制字母大小写均可。**通配符只能出现在半字节位置**：`?` 必须成对（`??`）或与一个十六进制字符搭配（`4?`），每个字节都要写满两个半字节。

某个字节只写了**一个**半字节（如 `4` 或 `?5?`）通常会解析失败并返回 `nullptr`——**但有一种例外要当心**：那多出来的半个若紧跟十六进制字符（中间没有空格），它会被当成下一个字节的第一个半字节吞掉，**不报错也不返回 nullptr**：

| 写的 | 结果 |
|---|---|
| `4` / `?5?` / `48 8B 4` | 解析失败（返回 `nullptr`） |
| `?5? 48 8B` | 解析失败（返回 `nullptr`） |
| **`?5?4 8B`** | **解析成 `?5 ?4 8B`——你以为的 `?5?` 变成了 `?5` 加 `?4`** |

最后一行是静默错配：不报错，但匹配的是另一个模式。所以字节之间可以省略分隔符（`488B??` 等价于 `48 8B ??`），但**不要让任何一个字节只写一个半字节**。

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

正确做法是找到某个会接收该对象作为参数的函数，用 `RegisterGlobalCapture` 拦截寄存器传参（仅前 4 个寄存器参数 `rcx`, `rdx`, `r8`, `r9`，对应 paramIndex 0~3）。这是**整数参数**：浮点/向量参数走 xmm0–3，不在可捕获范围内。

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

Stub 做的是**无痕中转**：被挂钩函数看到的参数寄存器（`rcx`/`rdx`/`r8`/`r9` 与 **`xmm0`–`xmm3`**）、`rsp` 与栈内容，都和没挂钩时一样。慢路径虽然要调一次 C 函数（`CaptureDispatch`），但在调用前后完整保存/恢复了这四个向量寄存器——它们是前四个浮点参数的传入位置。会被打掉的只有 `rax`/`r10`/`r11` 这类非参数寄存器，按调用约定函数入口本来就不该依赖它们的值。

捕获写入发生在调用目标函数**之前**，所以 `*out` 里拿到的是**本次调用传入**的那个参数；provider 第一次被调用之前，它仍是你初始化的 `nullptr`。订阅是**追加式**的：同一地址可以挂多个插件的 `out`，后注册不会顶掉先注册，触发时统一写入。

**提前撤销：`UnregisterGlobalCapture(providerAddress, out)`**。匹配条件是 (provider, out, 你的插件) 三者同时相同——别人的槽撤不掉，参数写错只会返回 `false` 并记日志。它**只摘订阅、不摘钩**：站点与 stub 永不释放，最后一个订阅移除后该地址立刻回到无捕获的快路径。插件卸载/隔离时 Loader 会按 owner 自动清掉剩余订阅，所以这是可选的提前释放手段（例如你只是在某个阶段需要那个指针）。

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

### 5.8 把 RVA 反查成符号名：`tools/rva_to_symbol`

仓库自带 `tools/rva_to_symbol.exe`（单文件，符号表在编译期嵌进去，不需要 Rust 环境）：
这个工具输入RVA就能获取到Loader内嵌符号库中对应的符号。
**注意：反编译不可能恢复所有符号，所以有符号名字和函数实际作用不一致是正常的。只要RVA能有个唯一对应的符号就行了，这份符号库未来大概率不会再更新，所以错误符号也不会有解析错误的风险（官方更新除外）。当然，如果你觉得错误符号会造成阅读困难，也可以直接使用RVA。**

```text
tools\rva_to_symbol.exe -gamecore 0x5BDAC0
tools\rva_to_symbol.exe -dx11 0x4D0D70
tools\rva_to_symbol.exe -dx12 0x37AB0 0x37DE0
tools\rva_to_symbol.exe --list
```

| 选择符 | 映像 | 对应 `HookImageId` |
|---|---|---|
| `-gamecore` | `GameCore_XP2_FinalRelease.dll` | `kHookImageGameCoreXP2` |
| `-dx11` | `CivilizationVI.exe` | `kHookImageExeDx11` |
| `-dx12` | `CivilizationVI_DX12.exe` | `kHookImageExeDx12` |

**输入是 RVA，不是绝对地址**。IDA/x64dbg 里拿到的是绝对地址时要先减映像基址 `0x140000000`：
`0x1404D0D70 - 0x140000000 = 0x4D0D70`。三个映像的 RVA 空间互不相通，所以必须显式给选择符——
工具不会给你一个"默认映像"。

输出固定四栏：`kind` / `demangled` / `mangled` / `vtable_xrefs`，也可以用 `--demangled` 之类只取一栏；
查不到打印 `0x...: not found`（退出码 1）。典型用法：

```text
# 只想知道名字，方便写 ResolveByName：
tools\rva_to_symbol.exe -gamecore 0x5BDAC0 --demangled
#   -> GameCore::Lua::Utility::RegisterScriptData

# dx11 的 pRegister（引擎注册 Modding 脚本库的入口）：
tools\rva_to_symbol.exe -dx11 0x4D0D70 --demangled
#   -> AppLuaModdingLibrary::pRegister

# 一次查多个 RVA：
tools\rva_to_symbol.exe -dx12 0x37AB0 0x37DE0 --demangled
```

拿到符号名之后，按 §5.2 写成 `HookName("GameCore::…")`，跨版本就交给 `PdataMatch` 矫正，尽量使用demangled符号，这样才能达到易读的目的，并且建议在注释里写上RVA，方便后续核查。

> 符号名来自 IDA 符号库，是**某一次分析**的快照，不是游戏官方导出。DX12 的 IDB 符号覆盖比 DX11稀。

### 5.9 函数序言校验（Prologue Check）：**慎用**

> **术语说明**：本文所说的「函数序言」（function prologue）指函数入口处的前几条指令，通常用来建立栈帧、保存寄存器。旧版文档里曾把它叫「前导字节」，那不是规范译法，现已统一为「函数序言」。

有些插件会在解析出地址后，再比对目标函数序言处的机器码（例如「前 8 字节必须是 `48 89 5C 24 08`」），对不上就判定「游戏更新了」并放弃功能。

**这个做法不是不能用，但必须清楚它的失效条件。** 本工程目前已从所有模块中移除，原因如下。

#### 主要风险：函数序言会被其它插件改写

函数序言不是稳定标识。任何人只要在该函数上下过 hook，入口就会被 MinHook 改写成跳转形态：

```asm
; 原始序言
48 89 5C 24 08    mov [rsp+8], rbx

; 被 hook 之后
E9 xx xx xx xx    jmp <trampoline>     ; 前 5 字节已被替换
```

于是**后加载的插件读到的根本不是原始字节**，校验必然失败。

#### 连带影响：顺序依赖的连锁失效

真实事故：本工程的一个模块先对 `AddGreatWorkToSlot` 装钩，另一个模块随后做序言校验，读到被改写的入口 → 判定 `mismatch` → 该模块把整组符号标记为「不完整」→ 所有 Lua API 集体返回 `false`。

症状看起来像「游戏更新了」，实际是**插件之间互相干扰**，而且**谁能通过校验取决于加载顺序**——这种隐性耦合极难排查。

#### 更麻烦的是：失败原因无法区分

序言校验的初衷是「符号库跨版本对齐可能把名字对到错误函数上」。但失败时有两种可能，而**表象完全相同**（都是 `mismatch`）：

| 实际情况 | 序言校验的表现 | 是否应该放弃功能 |
|---|---|---|
| 符号对齐错了，指向别的函数 | 不匹配 | ✅ 应该放弃 |
| 符号对齐正确，但入口被别人 hook 了 | 不匹配 | ❌ 不该放弃 |

你无法从 `mismatch` 本身判断属于哪一种。如果因此就关掉整个功能，等于用一个必然误杀的检查，去防一个低概率的问题。

#### 什么情况下可以谨慎使用

序言校验并非一无是处。以下条件**全部满足**时，它是合理的：

1. **该函数不会成为其它插件的 hook 目标**（例如纯数据访问的叶子函数、`retn` 包装器）；
2. **校验时机足够早**——在 `OnLoad` 里、且你确信此前没人改过它；
3. **失败只降级、不连带**——序言不匹配时只关闭这一项功能，不要把它当作「符号库坏了」的证据去关掉整组功能；
4. **特征码足够长且稳定**，并且已确认在当前 IDB 内唯一（太短会撞车，`48 89 5C 24 08` 这种在很多函数上都成立）。

反之，只要目标函数**可能被别的插件 hook**，就不要用它做判定。

#### 更可靠的替代手段

| 目的 | 推荐手段 |
|---|---|
| 跨版本定位 | `HookInfo` 的符号名主路径（Loader 的 `PdataMatch` 对齐） |
| 兜底 | AOB 特征码，且必须确认**在当前 IDB 内唯一命中** |
| 确认目标是「活的」 | 调用后做轻量运行期自检（返回值范围、空指针、枚举合法性） |
| 判断符号库是否可用 | `GetSymbolDbState`（见 §5.7），它给的是**全局状态**而非单点字节比对 |

#### 本工程的接口现状

`Common/GameCoreResolve.h` 里的 `gc::ResolveVerified<T>(api, image, info, sig, sigLen, label)` 仍然保留 `sig` / `sigLen` 两个参数，纯粹为了**不改动各模块的调用代码**——它们现在完全不被使用，函数只做「解析 + 失败打日志」。

新代码请直接调用：

```cpp
void* fn = gc::Resolve<void*>(api, kHookImageGameCoreXP2, kTargetMyFunc, 16);
if (!fn) {
    api->Log("%s", "[MyFeature] target unresolved - skipped");
    return;
}
```

`gc::PrologueMatches` / `gc::PrologueMatchesMasked` / `gc::ParseAob` / `gc::ResolveVerifiedAob` 已从代码库中移除。若某个功能确实需要序言校验，请按上文「什么情况下可以谨慎使用」的条件自行加回，并确保失败只降级、不连带。
---

## 6. HookAPI 核心功能与使用示例

本章按功能分组：§6.1 是所有插件都需要的初始化模板；§6.2–§6.3 是最常用的两种改游戏逻辑的手段（挂钩子与改字节）；§6.4–§6.8 是 Lua 与工具接口；**§6.9 是日志分级（热路径日志必须上闸）**；**§6.10 是进阶内容**——只有当普通 C++ detour 表达不了你的需求时，才需要读它。

### 6.1 基础入口与兼容性防御

```cpp
static HookAPI* g_api = nullptr;

extern "C" __declspec(dllexport) int OnLoad(HookContext* ctx) {
    // 必须首先检查 ABI 兼容性；返回 0 = 初始化失败
    if (!HookContext_IsCompatible(ctx)) {
        return 0;
    }

    HookAPI* api = HookAPI_Resolve(ctx);
    // 只列本插件真正依赖的入口；缺一个就说明 Loader 版本不匹配，干净退出
    if (!HookAPI_RequireAll(api, &HookAPI::Log)) {
        return 0;
    }
    g_api = api;

    api->Log("MyPlugin: OnLoad successfully initialized.");
    return 1;   // 初始化成功
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

**detour 必须是插件 DLL 里的真实函数。** Loader 会对 detour 地址做模块归属校验，运行期 `VirtualAlloc` / JIT 出来的地址会被拒绝。确实需要用机器码当 detour（改写寄存器、跳回函数中段）时，见 §6.9。

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
2. **已被挂钩的点位不能打补丁。** Loader 双向禁止这种组合（MinHook 会改写目标序言，两边各存一份"原始字节"必然互相覆盖）：目标 32 字节前导窗口内有钩子 → `Patch` 拒绝；反过来，已有补丁的区间 → `InstallHook` 拒绝。要改一个已挂钩的函数，请在 detour 里实现。
3. **不同插件可以叠加补丁。** 每个 target 维护一个按应用顺序的 owner 栈，每层存的是"自己写入之前"的字节。因此 `Unpatch` 还原的是你打补丁时看到的状态，不会踩掉别人后打的补丁。
4. **按"生效后不还原"来设计。** 把补丁当作在进程生命周期内持续有效，不要在卸载或换局时去撤销它。Loader 本身也不做任何自动回滚。
5. **能装钩子就别打补丁。** 字节补丁与你抄下来的那个游戏版本绑定，更新后该处指令一变，写进去的就是垃圾；`InstallHook` 走 `PdataMatch` + AOB 双重定位，能跟随更新。
6. **函数序言校验要慎用。** 见 §5.9：函数入口会被 hook 改写成 `E9 xx xx xx xx`，此时"比对原始入口字节"必然失败，而失败原因与"游戏更新"无法区分。除非确定该函数不会被别人 hook，否则不要用它做定位判定。

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
api->PushLuaMethods("GameCore::Lua::IQuestsManager::PushMethods", kHookApiPushNoManager, s_methods, 1);
```
之后在 Lua 脚本中即可直接调用：`Players[0]:GetQuestsManager():CustomMethod()`。

几点要说清楚：

- **第一个参数是完整符号名**，原样交给符号解析，Loader 不做任何拼接或推导——
  目标函数叫什么就写什么；
- **`sigType` 必须与该 PushMethods 的实际签名一致**：
  `kHookApiPushNoManager` = `PushMethods(lua_State*, int)`；
  `kHookApiPushWithManager` = `PushMethods(Manager*, lua_State*, int)`。
  填错不会报错，但参数会整体错位——用 `tools/rva_to_symbol.exe` 解出地址后反汇编确认第一个参数。

### 6.6 注册脚本全局环境（三个入口，按作用域选）

往 Lua 全局表 `_G` 里注入自定义模块。三个入口的差别只在**挂到哪个 Lua 环境**、以及**什么时候跑**：

| 入口 | 类别 | 挂到哪 | 生命周期 |
|---|---|---|---|
| `RegisterGameCoreScriptForGamePlay` | GameCore 专属 | GameCore 的 gameplay Lua | 每局重建，随对局卸载失效 |
| `RegisterGameCoreScriptForUI` | GameCore 专属 | GameCore 的 UI 脚本 Lua | 同上（与 gameplay 不是同一个 state） |
| `RegisterGlobalScript` | **Global 专属** | exe 建的**每一个** Lua 环境（`FrontEnd` / `MainMenu` / `Options` / 对局 …） | 进程级；每个环境各跑一次 |

回调签名统一是 `void (*)(void* L)`，`L` 就是对应的 `lua_State*`，可以直接用 Lua C API：

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

// 对局内脚本用（GameCore 插件）：
api->RegisterGameCoreScriptForGamePlay(&InitGlobals);
// GameCore 侧 UI 脚本用（GameCore 插件）：
api->RegisterGameCoreScriptForUI(&InitGlobals);
// 进程级全局用（仅 Global 插件可见）：每个 Lua 环境各跑一次
api->RegisterGlobalScript(&InitGlobals);
```

`RegisterGlobalScript` 的几个要点：

- **每个 Lua 环境都会跑一次**：引擎给每个脚本环境单独建 `lua_State`（`FrontEnd`、`MainMenu`、
  `Options`、`CreditsScreen`、对局的 gameplay …），各自有**独立的环境表**——挂在其中一个上的
  接口，别的环境**访问不到**。所以 Loader 会对每个环境各调一次你的回调，`L` 就是那一个环境；
- **每个 state 只跑一次**：同一个环境被重复触发（例如 UI 重建走同一条注册路径）不会重复执行；
  换局或重建环境（新的 state）会再跑一次；
- **在游戏线程执行**：回调里可以直接调 Lua C API，不需要自己切线程；
- **`L` 是当次环境自己的 state**，不是你事先取好的某个固定指针。想让接口在所有环境都可用，
  就直接往传入的 `L` 挂——Loader 会保证每个环境都被调用到；只想在某类环境生效，可以在回调里
  按需判断（例如比一下 `GetMainLuaState()`）；
- 插件卸载/隔离时 Loader 按 owner 自动摘除登记。

> 一个容易踩的点：**不要自己缓存第一次拿到的 `L` 然后反复往它上面写**。每个环境是独立的，
> 往旧的 `L` 写只影响那一个环境，新环境里照样看不见。正确的做法就是每次回调都往当次的 `L` 挂。

> ⚠ **第二个坑：回调里不要 raise Lua 错误。**
> `LuaError` / `LuaCheck*` 这类入口出错时走的是 longjmp，**不是普通返回**。
> Loader 在这些回调外面用 `__try/__except` 兜底，但 `__try` 接不住 longjmp ——
> 一旦 raise，Loader 帧里的 C++ 析构全部被跳过，在途计数永久不减，
> 之后换局或卸载时的 `WaitForDetourDrain` / `WaitAll` 会**永久阻塞**（表现为卡死）。
>
> 正确写法：**用 `LuaType` 判断类型，不合法就正常 `return`**，而不是让 Lua 抛错。
>
> ```cpp
> static int __fastcall lSetLevel(void* L) {
>     // 不要写 LuaCheckInteger(L, 1) —— 参数不对它会 raise
>     if (api->LuaType(L, 1) != 3) return 0;   // 3 = LUA_TNUMBER，静默忽略
>     const int n = static_cast<int>(api->LuaToNumber(L, 1));
>     // ...
>     return 0;
> }
> ```
>
> `GreatWorkAPI.cpp` 就是这么改的（见该文件头注释第 4 条）。

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
  void* pPlayer = api->GetPlayer(0); // 获取 0 号玩家指针（GameCore 专属）
  ```
- **开发者调试开关查询**：`IsDebugLogEnabled()` 见 §6.9。

### 6.9 日志分级：用 `IsDebugLogEnabled` 给热路径日志上闸

**这是硬要求，不是建议。** Loader 与所有插件共用同一个日志文件
（`Civ6DllLoader_Loader.log`），热路径上的日志会把它冲垮。

实测数据：一份 1884 行的日志里，有一个路线刷新回调独占 653 行、一个每
Lua state 注册一次的模块占 426 行，两者合计 **57%**。当时的情况是加载失败、
hook 装不上这类真正要看的行全被埋在中间。

#### 怎么判断一条日志该不该上闸

| 日志性质 | 要不要闸 | 例子 |
|---|---|---|
| 初始化一次、失败才打 | ❌ 不用闸，永远输出 | `hooked X at %p`、`registration FAILED`、`symbol missing` |
| 每次 hook 回调都打 | ✅ 必须闸 | 每个 detour 体的入口/出口日志 |
| 每帧 / 每次刷新 / 每次创建都打 | ✅ 必须闸 | 路线刷新、需求求值、工厂 Create |
| 玩家看得见的状态变化 | ✅ 必须闸 | 改了什么值、切了哪个模式 |

一句话：**频率与"每次游戏交互"相关的日志都必须上闸；只有"装好一次就没声音"的
日志可以裸打。**

#### 写法

```cpp
HookAPI* g_api = nullptr;

// 热路径日志开关。每次直接问 api，不要自己缓存 bool ——
// 缓存会多一个"什么时候缓存"的时序问题（OnLoad 之前还没拿到 api）。
inline bool DebugLogOn() {
    return g_api && g_api->IsDebugLogEnabled && g_api->IsDebugLogEnabled();
}

// 热路径：每次回调都触发
void __fastcall Hook_Something(void* self) {
    if (DebugLogOn()) {
        g_api->Log("MyFeature: Thing called self=%p", self);
    }
    ...
}

// 初始化路径：装一次，不用闸
g_api->Log("MyFeature: Detour_Something installed at %p", g_detour);
```

#### 开关语义

- 值来自 `<exe目录>\Civ6DllLoader\config.ini` 的 `[Loader] ENABLE_DEBUG_LOG`；
- **默认 false**，进程内恒定（改 ini 需要重启游戏才生效）；
- **只读、只能手改**：没有 Writer、没有 UI、没有 Lua 接口，插件也不该去改它；
- 键缺失 / 值非法一律读作 `false`（fail-closed）。

#### 别踩的坑

- **不要用 `if (g_api)` 代替 `if (DebugLogOn())`**：前者只防空指针，日志照样刷屏。
- **不要把初始化日志也塞进去**：那些行本来就只有一条，上闸反而让"为什么没打出来"
  变成新的排查方向。
- **热路径里别做字符串格式化**：`g_api->Log` 是 varargs，参数在调用前就求值了。
  拼字符串要放进闸里，否则闸没开也付了格式化的代价。

### 6.10 用 asmjit 生成 detour（进阶）

有些 hook 不能用普通 C++ 函数当 detour：需要在进入原函数前改写寄存器、按条件跳回原函数中间、或者把一段极短的逻辑塞进目标函数的指令流里。这时可以用 asmjit 在运行期汇编出这段机器码。

> **本章代码按 asmjit 1.17 写**（即本仓库 vendored 的版本，`ASMJIT_LIBRARY_VERSION` = 1.17.0）。
> 1.20+ 改了若干 API 名字（`kErrorOk`→`Error::kOk`、`codeSize()`→`code_size()`、
> `errorAsString()`→`error_as_string()`），直接照抄会编译不过 —— 对照表见 §6.10.9。

**这不是必需手段。** 只要普通 C++ detour 能满足需求，就用普通 detour——少一层机器码就少一类崩溃。

---

#### 6.10.1 硬约束一：detour 必须落在插件自己的模块内

`InstallHook` 在安装前会对 **detour 地址**做归属校验：用 `GetModuleHandleExW(FROM_ADDRESS)` 反查它属于哪个已加载模块，再确认该模块是本进程登记过的插件。

因此：

| detour 的来源 | 结果 |
|--------------|------|
| 插件 DLL 里的普通函数 | ✅ 通过 |
| 插件 DLL 里的自定义节（见下） | ✅ 通过 |
| `VirtualAlloc` / `JitRuntime::add` 返回的 JIT 页 | ❌ 被拒，日志：`InstallHook rejected: caller/callback is not an active plugin` |
| 堆内存 | ❌ 被拒 |

**asmjit 的 `rt.add(&fn, &code)` 返回的 `fn` 就在 JIT 页上，不能直接当 detour 用。** 正确做法是把它当作"编码器"：拿到字节后 `memcpy` 进插件模块内的存储，再 `rt.release(fn)`。

> 校验只针对 **detour 参数**。如果你不调 `InstallHook`，而是用 `Patch` 往游戏指令里写一条跳转、让控制流自己跳到 JIT 页，Loader 不检查跳转目标——但这属于另一条路线，见 §6.10.7。

---

#### 6.10.2 硬约束二：写入页和执行页要分开（W^X）

需要一块"能被写、又能被执行"的内存。**不要**做成永久 RWX（`PAGE_EXECUTE_READWRITE`，或 `/SECTION:.uhook,RWE`）：

- 任何一次越界写都能直接把数据变成可执行代码；
- 写坏了不会立刻崩，而是延迟到某次调用跳进坏指令，那时调用栈与被写坏的那行已经无关，极难定位。

正确的时间线是：

```
加载时   → RW    （能写，不能执行）
生成完   → RX    （能执行，不能写）
```

用自定义节实现：

```cpp
// 声明为可读可写，【不可执行】
#pragma section(".uhook", read, write)

// C++17 inline：多个 TU 包含该头文件时只保留一份定义
__declspec(allocate(".uhook")) alignas(4096) inline unsigned char g_detourCode[4096];
```

**不要**写 `#pragma section(".uhook", read, execute)` 配 `/SECTION:.uhook,RWE`——那是把节做成永久 RWX。也不要同时给 `write` 和 `execute`：MSVC 会报 C4330 并静默丢掉 `write`，最终得到的是 R+X 节，运行期一写就崩。

封页：

```cpp
DWORD old = 0;
VirtualProtect(g_detourCode, sizeof(g_detourCode), PAGE_EXECUTE_READ, &old);
FlushInstructionCache(GetCurrentProcess(), g_detourCode, sizeof(g_detourCode));
```

`FlushInstructionCache` 是**必须**的。x64 上不调用"通常也能跑"，但那是巧合，不是保证。

---

#### 6.10.3 硬约束三：emit 的错误必须逐条检查

这是最容易踩、且**完全没有报错**的坑。

asmjit 的每个 emit 调用都会返回 `Error`，出错时那一条指令**不写进缓冲区**。问题在于 `JitRuntime::_add()` 只检查扁平化、重定位和长度，**不检查 emitter 曾经报过错**——它照样返回成功。

所以只要有一处没检查返回值，就会出现：hook 体少了一条指令，却仍被装上。行为错乱，日志里一个字都没有。

典型触发场景（x86-64 编码本身的限制，不是 asmjit 的 bug）：

| 写法 | 结果 |
|------|------|
| `a.cmp(x86::rbx, 0x1_0000_0000ULL)` | ❌ `cmp r/m64, imm64` 不存在，只有 `imm32`（符号扩展） |
| `a.cmp(x86::rbx, 0x1234)` | ✅ 落在 int32 内，正常编码 |
| `a.mov(x86::rax, 0x1_0000_0000ULL)` | ✅ `mov r64, imm64`（`B8+` 形式）存在 |

所以每一条 emit 都必须检查：

```cpp
#define ASMJIT_EMIT(expr)                                                        \
    do {                                                                         \
        asmjit::Error _e = (expr);                                               \
        if (_e != asmjit::kErrorOk) {                                            \
            api->Log("[Hook] asmjit emit failed @%d (%s): %s",                   \
                __LINE__, #expr, asmjit::DebugUtils::errorAsString(_e));         \
            return false;                                                        \
        }                                                                        \
    } while (0)
```

> 名字必须是 `asmjit::kErrorOk` 与 `asmjit::DebugUtils::errorAsString`。本仓库 vendored 的是
> asmjit **1.17**，其中 `using Error = uint32_t;`（**不是** class），错误码在 `enum ErrorCode` 里 ——
> 写 `asmjit::Error::kOk` / `error_as_string` 在 1.17 下编译不过（那是 1.20+ 的写法）。
> 仓库里可直接抄的定义见 `UnlockModelLimit_Common.h` 的 `ASMJIT_EMIT`。

> 注意这个宏只能用在返回 `bool` 的函数里（失败路径是 `return false`）。返回 `void` 的函数要单独展开写。

---

#### 6.10.4 完整骨架

```cpp
// 本仓库 vendored 的是 asmjit 1.17：用 asmjit/x86.h（它内部已包含 core.h）。
// 下面代码按 1.17 的 API 写；1.20+ 的差异见 §6.10.9 的对照表。
#include "asmjit/x86.h"

// 进程内共用一个 JitRuntime 即可；它只负责"给 CodeHolder 分配一块临时可执行页"。
static asmjit::JitRuntime g_rt;

// ---- 生成 + 写入 + 封页 + 安装 ----
inline bool InstallAsmDetour(HookAPI* api, void* target, void** original,
                             asmjit::CodeHolder& code) {
    void* fn = nullptr;
    asmjit::Error err = g_rt.add(&fn, &code);
    if (err != asmjit::kErrorOk || !fn) {
        api->Log("asmjit jit failed: %s", asmjit::DebugUtils::errorAsString(err));
        return false;
    }

    const size_t size = code.codeSize();           // 1.20+ 是 code.code_size()
    bool ok = false;
    if (size > 0 && size <= sizeof(g_detourCode)) {
        DWORD old = 0;
        if (VirtualProtect(g_detourCode, sizeof(g_detourCode), PAGE_READWRITE, &old)) {
            memcpy(g_detourCode, fn, size);
            if (size < sizeof(g_detourCode)) {
                memset(g_detourCode + size, 0xCC, sizeof(g_detourCode) - size);  // 尾部填 int3
            }
            // 必须在 InstallHook 之前切到 RX：入口一旦被改写，随时可能有人跳进来
            if (VirtualProtect(g_detourCode, sizeof(g_detourCode), PAGE_EXECUTE_READ, &old)) {
                FlushInstructionCache(GetCurrentProcess(), g_detourCode, sizeof(g_detourCode));
                ok = api->InstallHook(target, g_detourCode, original);
            }
        }
    } else {
        api->Log("hook code size %zu exceeds slot capacity %zu", size, sizeof(g_detourCode));
    }

    g_rt.release(fn);                 // 字节已拷走，JIT 页立刻还掉
    return ok;
}

// ---- 调用方（在 OnLoad 里）----
asmjit::CodeHolder code;
code.init(g_rt.environment());
asmjit::x86::Assembler a(&code);
ASMJIT_EMIT(a.push(asmjit::x86::rcx));
ASMJIT_EMIT(a.mov(asmjit::x86::rax, (uint64_t)someAbsoluteAddress));  // 绝对地址一律用立即数
// ...
InstallAsmDetour(api, target, &g_orig, code);
```

注意骨架里 `InstallAsmDetour` 返回的 `bool` 要真的检查。装不上就应当让 `OnLoad` 返回失败（或至少记一条日志），不要静默继续——一个"看起来装上了、实际每次调用都走原函数"的 hook 比崩溃更难查。

---

> **如果是 Global 插件**：detour 代码会随模块一直驻留到进程退出，上面这套"写进自定义节"是安全的。
> **如果是 GameCore 插件**：模块会在返回主菜单/换局时被真正卸载。Loader 在卸载前会先批量还原入口字节，
> 因此正常情况下不会再有人跳进你的 detour；但**不要**在插件自己的清理路径里提前释放或改写这块代码。

---

#### 6.10.5 多条 detour 时：一个槽一页

`VirtualProtect` 按内存页（4 KB）生效。如果多个 detour 挤在同一页：

- 装第 2 条时把页切回 RW；
- 已经在执行的第 1 条 detour 所在页同时变成不可执行 → 崩溃。

要么给每个槽独立一页：

```cpp
__declspec(allocate(".uhook")) alignas(4096) inline unsigned char g_slot0[256];
__declspec(allocate(".uhook")) alignas(4096) inline unsigned char g_slot1[256];
__declspec(allocate(".uhook")) alignas(4096) inline unsigned char g_slot2[256];
__declspec(allocate(".uhook")) alignas(4096) inline unsigned char g_slot3[256];

inline unsigned char* const g_slots[4] = { g_slot0, g_slot1, g_slot2, g_slot3 };
```

这样切换第 2 个槽的权限时，第 1 个槽所在的页完全不受影响。

要么一次性生成全部 detour、最后统一封页：适合所有 detour 都在 `OnLoad` 里装完、之后不再改的场景。**不要**在"装完一批、封页"之后又回头去改同一页。

> 把 `alignas(4096)` 写进同一个数组（例如 `char g[4][256]`）不行——四段会连续排布、跨页，等于共用页。必须拆成四个独立变量，让链接器给每个都做页对齐。

---

#### 6.10.6 生成代码里不要出现"随基址变化"的引用

`rt.add` 会把代码重定位到 JIT 页的地址，然后你把字节拷到别的地方。因此：

- ✅ 绝对地址用立即数：`a.mov(reg, (uint64_t)ptr)`
- ✅ 代码块内部的相对跳转（`a.jmp(label)`、`a.je(label)`）：整块一起搬，相对距离不变，安全
- ❌ 引用"标签自身的绝对地址"、使用地址表：重定位时按 JIT 地址算好，搬走后就是错的

需要绝对地址时，一律 `(uint64_t)` 强转后当立即数嵌入。

另外，x86-64 **没有** `call imm64` 这种编码。要调绝对地址，必须经寄存器中转：

```cpp
ASMJIT_EMIT(a.mov(asmjit::x86::rax, (uint64_t)helper));
ASMJIT_EMIT(a.call(asmjit::x86::rax));
```

---

#### 6.10.7 什么时候根本不需要 detour

如果需求只是"改掉某条指令里的一个常量"或"把一处条件跳转 nop 掉"，用 `Patch` 直接改字节即可，不需要 asmjit，也不需要自定义节：

```cpp
static uint8_t kNewBytes[] = { 0x48, 0xC7, 0xC0, 0x00, 0x02, 0x00, 0x00 };
api->Patch(target, kNewBytes, sizeof(kNewBytes));
```

asmjit 在这种场景下唯一的价值，是让你不用手写那串十六进制——生成完 `memcpy` 进一个普通缓冲区再交给 `Patch` 就行，**不需要 `.uhook` 节，也不需要过 detour 归属校验**。

判断标准很简单：

| 你需要 | 方案 |
|--------|------|
| 在函数入口前后插逻辑 | 普通 C++ detour（§6.2） |
| 改一条指令的立即数 / 操作码 | `Patch`（§6.3），字节可以手写也可以用 asmjit 生成 |
| 改写寄存器、按条件跳回函数中段 | asmjit + 自定义节（本章） |

---

#### 6.10.8 工程配置

| 项 | 值 |
|----|----|
| 平台 / 配置 | `x64` / `Release`（与插件其余部分一致） |
| 预处理宏 | `ASMJIT_STATIC`（**必需**；缺了会把 asmjit 当 DLL 导入，报一堆 `__imp_` 未解析符号） |
| 附加包含目录 | asmjit 头文件所在目录 |
| 附加依赖项 | `asmjit.lib` |
| 运行库 | 必须与 `asmjit.lib` 的编译方式一致。混用 `/MT` 与 `/MD` 会报 LNK4098 并埋下跨 CRT 分配/释放的隐患 |

`ASMJIT_STATIC` 的作用只有一条：把 `ASMJIT_API` 从 `__declspec(dllimport)` 改成空。漏定义时所有 asmjit 符号都会变成"从 DLL 导入"，链接期报 `error LNK2019: 无法解析的外部符号 "__declspec(dllimport) ... asmjit::CodeHolder::CodeHolder"`。

asmjit 源码本身是 zlib 许可，直接 vendored 一份到插件工程里最省事（头文件 + 用 CMake 编一次 `asmjit.lib`，注意 `ASMJIT_STATIC=ON` 和 `CMAKE_MSVC_RUNTIME_LIBRARY` 要与插件一致）。不需要额外依赖——asmjit 不依赖任何第三方库。

---

#### 6.10.9 asmjit 版本差异

不同版本之间 API 命名变过，跨版本抄代码会编译失败：

| | 1.17 | 1.20 及以后 |
|---|---|---|
| 成功错误码 | `kErrorOk` | `Error::kOk` |
| 代码长度 | `code.codeSize()` | `code.code_size()` |
| 错误串 | `DebugUtils::errorAsString()` | `DebugUtils::error_as_string()` |
| 推荐头文件 | `asmjit/x86.h` | `asmjit/x86.h`（旧的 `asmjit/asmjit.h` 仍可用，但会打印 deprecated 提示） |
| ABI 命名空间 | `asmjit::v1_17` | 随版本变化（如 `asmjit::v1_21`） |

版本号在 `asmjit/core/api-config.h` 的 `ASMJIT_LIBRARY_VERSION`。**ABI 命名空间是链接期的版本闸门**：头文件与 .lib 版本不一致时，符号落在不同命名空间，链接直接失败，而不是运行时错乱。

不同插件、插件与 Loader 之间各自静态链接自己的 asmjit，互不干扰。但**同一个插件内部**必须用同一份头文件与 .lib。

---

#### 6.10.10 排查表

| 现象 | 原因 |
|------|------|
| `InstallHook rejected: caller/callback is not an active plugin` | detour 不在插件模块内（多半是把 `rt.add` 的返回值直接传进去了） |
| 链接报 `__imp_...asmjit...` 未解析 | 忘了定义 `ASMJIT_STATIC` |
| 链接报 `LNK4098` 默认库冲突 | 插件运行库（`/MT`、`/MD`）与 `asmjit.lib` 不一致 |
| 装上后行为错乱、无任何日志 | emit 失败被静默丢弃，没做 `ASMJIT_EMIT` 检查 |
| 第一条 detour 正常，装第二条后崩 | 多条 detour 共用一页，封页/回写互相打断 |
| 首次调用就访问违例 | 页面还是 RW 就被执行；或 `InstallHook` 在切 RX 之前就调用了 |
| 偶发崩溃、重启后不复现 | 缺 `FlushInstructionCache`，或生成代码里嵌了随基址变化的绝对引用 |
| 换了一台机器/重编后 hook 装不上 | 节被优化掉（常量未使用），或 `alignas` 没写导致槽跨页 |

自检：封页之后可以确认页面属性

```cpp
MEMORY_BASIC_INFORMATION mbi{};
if (VirtualQuery(g_detourCode, &mbi, sizeof(mbi))) {
    // 期望 PAGE_EXECUTE_READ（0x20）；若仍是 PAGE_READWRITE(0x04) 说明封页失败
}
```

---

## 7. 添加主菜单 UI（FrontEnd）

前面六章讲的都是**插件 DLL**。这一章不一样：**给主菜单加一块自己的界面**，只要写文件，不用编译 DLL、不用 `OnLoad`、也不用管 `CIV6_PLUGIN_*` 宏。

你要做的事只有一件：写一对同名的 `FrontEndUI_*.xml` + `.lua`，**放进 Mod 目录里就行**（不必在 `.modinfo` 里登记）。剩下的由 Loader 完成——它按文件名在你的 Mod 目录树里找，主菜单建好后把界面挂进去，用不着你写一行 C++。

> 适用场景：**主菜单**（FrontEnd）上的面板、按钮、弹窗。对局内的界面（InGame）不走这条路，仍要用原生
> `AddUserInterfaces` + `ReplaceUIScript` 声明。

### 7.1 文件与命名

```text
MyAwesomeMod/
  MyAwesomeMod.modinfo          ← 只用于 Mod 自身的启用/元数据，Loader 不读它
  UI/FrontEnd/
    FrontEndUI_MyMod.xml        ← 界面定义
    FrontEndUI_MyMod.lua        ← 逻辑，与 xml 同名同目录
  Binaries/Win64/
    GameCore_XP2_Shell_FinalRelease.dll
    Civ6DllLoader.runtime.dll
```

文件名不能随便起，**不满足下面任何一条，这个界面就不会被加载**（日志里只有"发现 N 个"的计数，不会指出是哪一个被跳过）：

| 要求 | 说明 |
|---|---|
| 前缀必须是 `FrontEndUI_` | 这是 Loader 的识别标志，大小写不敏感 |
| 扩展名必须是 `.xml` | 只找 xml；同名的 `.lua` 会自动一起加载 |
| 前缀后面至少还有一个字符 | `FrontEndUI_.xml` 不合法 |
| 必须放在 Mod 目录里 | Loader 只扫已启用 Mod 的目录树，递归最多 8 层 |

`.xml` 与 `.lua` 必须**同名同目录**。放在 Mod 里哪个子目录都行——Loader 取的是**相对 Mod 根的路径**（分隔符统一成 `/`、去掉 `.xml`）交给游戏加载，所以 `UI/FrontEnd/FrontEndUI_MyMod.xml` 与 `FrontEndUI_MyMod.xml`（直接放 Mod 根）都能用。两个 Mod 里相对路径相同的文件只加载一份。

### 7.2 不需要在 .modinfo 里声明

Loader **不读** `<FrontEndActions><ImportFiles>`。把 `FrontEndUI_*.xml` 放进 Mod 目录即可，写在 `.modinfo` 里的 `ImportFiles` 对 Loader 没有任何影响。

> 本仓库 `docs/template/YourDllMod.modinfo` 里**确实写了**一段 `<ImportFiles id="FrontEndUI">`。
> 那段是给**不用 Loader、走游戏原生 ForgeUI 加载**的场合留的；用 Loader 时它既不生效也不碍事。
> 换句话说：**加不加都行，Loader 一律按文件名扫描。**

这样做的副作用只有一个：**同一个 Mod 目录里凡是叫 `FrontEndUI_*.xml` 的文件都会被加载**。不要拿这个前缀给别的 xml 起名。

### 7.3 不支持条件加载

**这条路注入的界面一定会加载，不能加条件。** Loader 只看文件名，`.modinfo` 里的任何 `criteria` 都不参与这个判断。

界面若确实需要按条件出现，两条路：

- 让界面**总是注入**，在 `.lua` 里自己判断该不该显示（`ContextPtr:SetHide(true)`）；
- 改用对局内的原生 UI 机制（`AddUserInterfaces` + `ReplaceUIScript`），那条路支持 `criteria`。

### 7.4 Mod 的状态要求

- Mod 必须**已启用**（游戏里勾掉的不算）：Loader 只扫 `Mods.sqlite` 里登记为启用的那些 mod 目录；
- Mod 目录本身必须可读；扫描遇到无权限的子目录会跳过，不会中断其它 Mod 的加载。

### 7.5 出问题时看日志

进主菜单后打开 `Civ6DllLoader_Loader.log`，搜 `UiInject:`。正常应当看到两行：

```
UiInject: 发现 2 个 Mod UI
UiInject: context 已加载 (FrontEndUI_MyMod)
```

对照着定位：

| 日志情况 | 问题在哪 |
|---|---|
| 没有 `发现 N 个 Mod UI` | Mod 没被 `Mods.sqlite` 判为启用（§7.4），或扫描阶段整体出错（日志里有原因） |
| 自己的诊断日志一条都没有 | 是不是套了 `DebugLogOn()`？那是按 `ENABLE_DEBUG_LOG` 上闸的，默认关（§6.9） |
| 有 `发现 N 个`，但没有你自己那个文件名 | 文件名前缀/扩展名不合规，或放到了未启用的 Mod 目录里——对照 §7.1 的表 |
| 有 `context 已加载 (你的文件名)`，界面却不显示 | 问题在界面自己（XML 布局或 Lua 没显示它），与 Loader 无关 |

### 7.6 注意

- **只覆盖主菜单**。对局内界面不在此列（见 §7 开头）。
- **无条件加载**（见 7.3）；`.modinfo` 不需要写 `ImportFiles`（见 7.2）。
- **界面会随主菜单重建而重建**。极少数情况下这一次不会重注，界面要等下一次主菜单重建才回来；重启游戏必定恢复。

---

## 8. 常见问题排查（FAQ）

### Q：玩家需要单独订阅或下载前置 Loader 吗？
**A：不需要。** 每个采用本体系的 Mod 都按规范在 `Binaries/Win64/` 自带薄壳与 `Civ6DllLoader.runtime.dll`。若玩家首次安装 Mod，薄壳会自动完成 `version.dll` 接管引导；若已存在 `version.dll`，则始终进入稳态运行。

### Q：多个不同 Mod 都带了 `Civ6DllLoader.runtime.dll`，会发生版本冲突吗？
**A：不会。** Loader 的竞选机制会自动比对所有已启用 Mod 中的 runtime，**始终选择版本最高的 runtime.dll 进行加载**。因此较新发布的 Mod 可以无缝带动老 Mod 享用更新的 Loader 功能与修复。

### Q：我的插件放到 `Binaries/Win64/Plugins/` 下为什么没有加载？
**A：先查两个门禁。**
1. **目录**：Loader 严格只扫描 `Plugins/Global/*.dll` 和 `Plugins/GameCore/*.dll`。直接散落在 `Plugins/` 根目录下的 DLL 不认。
2. **版本资源**：DLL 必须带 `VERSIONINFO` 文件版本（见 §2.2）。没有的话日志里会有
   `Rejected plugin (no VERSIONINFO file version resource)`。

另外确认 Mod 本身已启用——Loader 只从 `Mods.sqlite` 记录的已启用 Mod 里找插件。

### Q：我的插件明明存在，但日志说被别的路径顶掉了？
**A：同名插件按版本选举，只留一份。** 多个 Mod 带了同名 DLL 时，Loader 只加载**文件版本最高**的那份，
其余的被淘汰。日志形如：

```
Duplicate plugin basename: kept <胜出路径> (v1.2.0.0), dropped <你的路径> (v1.0.0.0, lower version)
```

版本相同时按路径字典序裁决（`same version -> lexicographic tie-break`）。
把版本号提上去，或者换一个不会被撞到的文件名。详见 §2.2。

### Q：怎么让"基于同一份基础插件改的多个 Mod"共存？
**A：给同一个文件名填不同版本号即可。** Loader 会挑最高的那份加载，其余自动让位。
这也是推荐做法——只有一份映像在跑，不会出现两个 Mod 各挂一遍钩子互相干扰。

### Q：编译报错 `#error "CIV6: 必须声明插件类别"` 怎么解决？
**A：必须在工程预处理器中定义类别宏。** 在工程属性「C/C++ -> 预处理器 -> 预处理器定义」中添加 `CIV6_PLUGIN_GLOBAL`（全局插件）或 `CIV6_PLUGIN_GAMECORE`（对局插件）。

### Q：为什么调用 `RegisterGameCoreScriptForGamePlay` 或 `GetPlayer` 编译报错提示不是 HookAPI 成员？
**A：触发了编译期门禁保护。** 你的插件定义了 `CIV6_PLUGIN_GLOBAL`，而这些接口属于 GameCore 对局专属接口（生命周期受对局创建/销毁限制，常驻插件调用会发生空指针或非法访问）。如果确实是玩法逻辑插件，应转为 GameCore 插件（定义 `CIV6_PLUGIN_GAMECORE` 并放入 `Plugins/GameCore/` 目录）。

### Q：游戏更新后插件需要重新编译发布吗？
**A：通常不需要。** Loader 内置跨版本对齐（`PdataMatch`），插件若采用第 5 节推荐的 `HookInfo`、`ResolveByHookInfo` 或 `ResolveByName` 写法，旧 RVA 或符号名在更新后会自动矫正到新地址。若定位失败，可查看 `Documents/My Games/Sid Meier's Civilization VI/Logs/Civ6DllLoader_Loader.log` 中的警告。

### Q：`OnUnload` 里要做什么清理吗？
**A：不需要，保持空实现或直接不导出即可。** Loader 会自动回收钩子、参数捕获与消息订阅；退局或重载时，整个 GameCore 模块由引擎完整释放。详见 §3 的生命周期说明。

### Q：用 `Patch` 改了内存，卸载插件时会自动还原吗？
**A：不会。** Loader 不对补丁做自动回滚，写入的字节在进程退出前持续有效。因此补丁应按"生效后不还原"来设计。`Unpatch` 可用于运行期动态撤销，但不要把它当作卸载清理的常规步骤（详见 §6.3）。

### Q：只想给主菜单加个面板，也必须写 C++ 插件吗？
**A：不必。** 放一对 `FrontEndUI_*.xml` + `.lua` 进 Mod 目录即可，**不需要**编译 DLL，也**不需要**在 `.modinfo` 里写 `<ImportFiles>`——Loader 按文件名扫描（见 §7.2）。详见 §7。

### Q：主菜单 UI 没出现，怎么排查？
**A：** 依次核对——① 文件名是否以 `FrontEndUI_` 开头、以 `.xml` 结尾，且与 `.lua` 同名同目录；② 文件是否放在**已启用** Mod 的目录树里（递归上限 8 层）；③ `Civ6DllLoader_Loader.log` 里是否出现 `UiInject: 发现 N 个 Mod UI` 与 `UiInject: context 已加载 (你的文件名)`。

注意：`.modinfo` 里写不写 `<ImportFiles>` 对 Loader **没有影响**（§7.2）——不要照官方 UI 教程去登记，白花时间。详见 §7.5。

---

## 附录 A：HookAPI 接口全表

`HookAPI` 是插件侧的函数指针表，由 `HookAPI_Resolve(ctx)` 在 `OnLoad` 里按名字填好，
每格要么是有效函数地址、要么是 `nullptr`。以下按用途分组列出全部 80 个入口。

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
| `RegisterGlobalCapture` | `bool(void* providerAddress, uint32_t paramIndex, void** out)` | 拦截 `providerAddress` 的第 `paramIndex` 个**整数**寄存器参数（0–3 对应 rcx/rdx/r8/r9；浮点参数走 xmm，抓不到）写入 `*out`。同一地址可多插件叠加订阅。获取未导出全局对象的标准手段，见 §5.6。 |
| `UnregisterGlobalCapture` | `bool(void* providerAddress, void** out)` | 撤销本插件先前在该 provider 上的一个捕获订阅（三者匹配）。只摘订阅、不摘钩；最后一个订阅移除后回到快路径。见 §5.6。 |

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
| 表操作 | `LuaCreateTable` `LuaGetField` `LuaSetField` `LuaGetTable` `LuaSetTable` `LuaRawGetI` `LuaNext` |
| 引用 | `LuaRef` `LuaUnref` |

**遍历哈希表用 `LuaNext`**。`LuaObjLen` 只给序列部分的长度，拿不到字符串键。`LuaNext(L, index)`
是 Lua 5.1 `lua_next` 的直接封装，栈契约也照搬：

```cpp
// 假设要遍历的表在 tblIndex（负数索引会随压栈变化，先转成绝对索引）
const int t = tblIndex < 0 ? api->LuaGetTop(L) + tblIndex + 1 : tblIndex;
api->LuaPushNil(L);                        // 首轮：key 位置压 nil
while (api->LuaNext(L, t)) {               // 返回 1：栈上多了 key(-2) / value(-1)
    // LuaType 返回标准 Lua 5.1 类型码：字符串 = 4、数字 = 3（HookAPI 不额外定义常量）
    const char* k = api->LuaType(L, -2) == 4 ? api->LuaCheckLString(L, -2, nullptr) : nullptr;
    const double  v = api->LuaToNumber(L, -1);
    // ... 处理这一项 ...
    api->LuaPop(L, 1);                     // 关键：自己弹掉 value，只留 key 给下一轮
}
```

两个容易踩的点：

- **value 必须自己弹**。`LuaNext` 只负责弹出 key、压入下一组 key/value；不弹 value 会让下一轮
  从一个错的位置开始，遍历会跳项甚至死循环；
- **遍历中不要改表**。Lua 5.1 对"遍历时新增/删除键"的行为未定义；要改就先收集到自己的容器里，
  遍历结束后再写回。

按用途分组：

| 用途 | 入口 |
|---|---|
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
| `RegisterGameCoreScriptForGamePlay` ✱ | `void(ScriptGlobalsFn)` | 注册 GameCore gameplay Lua 构建完成时的回调。 |
| `RegisterGameCoreScriptForUI` ✱ | `void(ScriptGlobalsFn)` | 同上，但用于 GameCore 的 UI 脚本 Lua。 |
| `RegisterGlobalScript` | `void(ScriptGlobalsFn)` | 注册到 exe 的引擎主 Lua state（进程级，换局不失效）。Global 与 GameCore 插件都能用，见 §6.6。 |
| `PushLuaMethods` ✱ | `HookApiPushMethodsResult(const char* pushMethodsSymbol, int sigType, const LuaMethodInfo*, uint32_t count)` | 给已有 C++ Lua 对象挂方法，见 §6.5。 |

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

api->RegisterGameCoreScriptForGamePlay(&InitMyMod);   // Lua: MyMod.Add(1, 2) -> 3
api->RegisterGameCoreScriptForUI(&InitMyMod);          // GameCore UI Lua 里同样可用
```

```cpp
// 给一个引擎已有的 Lua 对象加方法（第一个参数是完整符号名，不自动拼接）：
api->PushLuaMethods("GameCore::Lua::IMyManager::PushMethods", kHookApiPushNoManager, s_methods, count);
// sigType：kHookApiPushNoManager（无 manager 前缀）/ kHookApiPushWithManager（带 manager 签名）
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
| `GetPlayer` ✱ | `void*(int playerId)` | 取玩家对象指针。对局开始前返回 nullptr。 |
| `MakeHash` | `uint32_t(const char*)` | 与引擎 `GameCore::Utilities::MakeHash` 逐位一致的 CRC32。**纯本地计算**，对局前/重载后都能用。 |
| `Log` | `void(const char* fmt, ...)` | 写 `Civ6DllLoader_Loader.log`。 |

```cpp
uint32_t h = api->MakeHash("QUEST_SEND_TRADE_ROUTE");   // 与引擎内部哈希一致
api->Log("hash of quest = 0x%08X", h);
```

> **关于 GameCore 锁**：`TriggerGameEvent`、`PushLuaMethods` 这类需要进 Lua 的高层接口，
> 锁的取放由 Loader 在内部完成，插件侧**没有也不需要**对应接口，绝大多数场景不必手动取锁。
> 插件若直接调用引擎内部函数，锁要自行负责，且**必须在 SEH 保护下做**——释放之后若取回时抛异常，
> 锁会永久不取回，游戏直接卡死。

### A.9 初始化与兼容性检查

```cpp
static inline int      HookContext_IsCompatible(const HookContext* ctx);
inline        HookAPI* HookAPI_Resolve(const HookContext* ctx);

template <typename... M>
inline bool HookAPI_RequireAll(const HookAPI* api, M... members);
```

| 头文件内的辅助工具 | 说明 |
|--------------------|------|
| `HookContext_IsCompatible(ctx)` | 检查 Loader 与插件是否同一次构建（比较 `structSize` + `apiVersion`）。`OnLoad` 开头调一次，返回 0 就 `return 0` 让加载失败。 |
| `HookAPI_Resolve(ctx)` | 按名字把 80 个入口解析进本模块私有的表，返回其指针。幂等；同一插件的多个 `.cpp` 共享同一份。 |
| `HookAPI_RequireAll(api, &HookAPI::A, &HookAPI::B)` | 检查指定入口是否都解析成功。只列自己用到的；缺项返回 false，用于优雅退出。 |
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

extern "C" __declspec(dllexport) int OnLoad(HookContext* ctx) {
    if (!HookContext_IsCompatible(ctx)) {
        return 0;
    }
    HookAPI* api = HookAPI_Resolve(ctx);
    // 必须把【真正用到的】每一个入口都列进来 —— 包括回调里间接调用的那些。
    // 下面这段 OnInitGlobals / lVersion 用到 Lua 表与字符串压栈，
    // 漏列即空指针调用（本文件 §3 的要点里说过这条，这里是最容易犯的地方）。
    if (!HookAPI_RequireAll(api,
            &HookAPI::Log,
            &HookAPI::RegisterGameCoreScriptForGamePlay,
            &HookAPI::LuaNewGlobalTable,
            &HookAPI::LuaPushCFunctionField,
            &HookAPI::LuaPushFString,
            &HookAPI::LuaPop)) {
        return 0;
    }
    g_api = api;
    api->Log("MyMod: loaded");

    api->RegisterGameCoreScriptForGamePlay(&OnInitGlobals);
    return 1;
}

// 可选：接收其它插件的消息
extern "C" __declspec(dllexport)
void OnMessage(const char* topic, const void* data, size_t len) {
    g_api->Log("MyMod: got topic=%s len=%zu", topic ? topic : "?", len);
}
```

**部署**：把编译出的 `MyGameplayPlugin.dll` 放进 `MyMod/Binaries/Win64/Plugins/GameCore/`。

**验证**：进对局后打开 FireTuner 执行 `print(MyMod.Version())`，应输出 `MyMod v1.0`；同时 `Civ6DllLoader_Loader.log` 中应出现 `MyMod: loaded`。
