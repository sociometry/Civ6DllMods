# Civ6DllLoader 插件开发教程

面向：**只想写自己功能 DLL 的作者**。无需阅读或修改 loader 源码；只要把两份固定 DLL 放进 mod，并按本文模板调用 `HookAPI`。

---

## 1. 架构简介

```text
游戏加载你的 mod
  → 薄壳 GameCore_XP2_Shell_FinalRelease.dll（劫持 GameCore 槽位）
  → 竞选并启动 Civ6DllLoader.runtime.dll（版本最高者获胜）
  → runtime 扫描各 mod 的 Plugins\*.dll
  → 调用你的 OnLoad(HookAPI*)
  → 你用 HookAPI 注册 Lua / 查函数 / 装 hook
```

| 文件 | 谁提供 | 你要做什么 |
|------|--------|------------|
| `GameCore_XP2_Shell_FinalRelease.dll` | loader 发行包（薄壳） | **原样复制**进 mod，不要改 |
| `Civ6DllLoader.runtime.dll` | loader 发行包（runtime） | **原样复制**；尽量用较新版本 |
| `Plugins\YourMod.dll` | **自己编译** | 写逻辑、导出 `OnLoad` |
| `HookAPI.h` | loader 发行包（复制进工程） | 唯一需要 `#include` 的头 |

多 DLL mod 同时启用时：谁最后改 `GameCores` 谁的薄壳被加载；薄壳会扫描兄弟 mod，选 **版本最高** 的 runtime，并加载 **所有** 启用 mod 的 `Plugins`。因此作者只需「自带两份必要 DLL」，不必要求玩家单独订阅前置。

---

## 2. Mod 目录模板

```text
MyAwesomeMod/
  MyAwesomeMod.modinfo          ← UTF-8 无 BOM
  Data/
    Config.sql                  ← PackageId = modinfo 的 id
  Binaries/Win64/
    GameCore_XP2_Shell_FinalRelease.dll
    Civ6DllLoader.runtime.dll
    Plugins/
      MyAwesome.dll             ← 你的插件
```

### Config.sql

```sql
UPDATE GameCores
SET
    PackageId = '你的-modinfo-GUID',
    DllPrefix = 'GameCore_XP2_Shell'
WHERE
    GameCore = 'Expansion2';
```

依赖 **风云变幻（Gathering Storm）**。

---

## 3. 工程最小要求

- 平台：`x64`，配置：`Release`（与游戏一致）
- 语言：C/C++，导出 **C 链接**
- 把发行包里的 `HookAPI.h` 完整复制进插件工程目录（本仓库各 recipe 的 `dll/` 已自带）
- **只** `#include "HookAPI.h"`

必须导出：

```cpp
extern "C" __declspec(dllexport) void OnLoad(HookAPI* api);
```

可选：

```cpp
extern "C" __declspec(dllexport) void OnUnload();
extern "C" __declspec(dllexport) void OnMessage(const char* topic, const void* data, size_t len);
```

若导出了 `OnMessage`，runtime 会按插件文件名（去掉 `.dll`）自动注册消息处理。

`OnUnload` / `DllMain(DETACH)` 尽量 **空实现**；不要在卸载时做复杂清理。

---

## 4. HookAPI 功能

`OnLoad` 拿到的 `HookAPI*` 是 runtime 提供的稳定接口表。先看最常用的几类。

### 4.1 注入全局 Lua 表（推荐入门）

| 接口 | 用途 |
|------|------|
| `RegisterScriptGlobals` | gameplay Lua 构建时回调 |
| `RegisterScriptGlobalsForUI` | UI/Cache Lua 构建时回调 |
| `LuaCreateTable` / `LuaSetField` / `LuaPushNamedCClosure` | 组表、挂方法 |
| `kHookApiLuaGlobal` | 等价 `_G`（`-10002`） |
| `GetCurTurn` | 当前回合 |
| `LuaCheckInteger` / `LuaOptInteger` / `LuaPushInteger` / … | 参数与返回值 |

适合：给 FireTuner / 自己的 UI Lua 暴露 `MyMod.DoSomething()`。

### 4.2 给已有 GameCore Lua 对象加方法

```cpp
const LuaMethodInfo methods[] = {
    { lFoo, "lFoo", "Foo" },
    { lBar, "lBar", "Bar" },
};
api->PushLuaMethods(
    "GameCore::Lua::IQuestsManager",  // 目标对象名（需 runtime 已登记）
    methods,
    2);
```

当前 runtime 已登记示例：

- `GameCore::Lua::IQuestsManager`（无 manager 前缀签名）
- `GameCore::Cache::Lua::IQuestsManager`（带 manager 签名）

用于：在 `Players[i]:GetQuestsManager():YourMethod()` 这类对象上挂方法。参考 `ChoosableCityStateQuests`。

`LuaMethodInfo` 必须是 **C 布局**（`const char*` 字面量），不要跨 DLL 传 `std::string` / `std::vector`。

### 4.3 解析 GameCore 内部函数

```cpp
using Fn = void(__fastcall*)(/* ... */);
Fn f = GetFunctionByName<Fn>(api, "GameCore::Quests::Manager::AttemptNewQuests");
// 也可用 MSVC 修饰名，或 IDA 风格 sub_18XXXXXXX
// 或
void* p = api->GetFunctionByAddress(0x64B400); // RVA，游戏版本敏感
```

`GetFunctionByName` 查 runtime 符号库（约 2.5 万+ unique + 别名）。优先用友好名；没有的名字可试修饰名，或向 loader 维护者在 `symbol_aliases.json` 补别名。

### 4.4 安装 MinHook

```cpp
static void* g_original = nullptr;
void* target = api->GetFunctionByName("...");
api->InstallHook(target, &MyDetour, &g_original);
```

### 4.5 插件间消息

```cpp
api->BroadcastMessage("my-topic", data, len);
api->SendMessageTo("OtherPlugin", "my-topic", data, len); // 名称为对方 dll 主文件名
api->RegisterMessageHandler("MyPlugin", &MyHandler);       // 也可手写；导 OnMessage 会自动注册
```

### 4.6 日志

```cpp
api->Log("hello %d", 42);  // 写入 Civ6DllLoader_Runtime.log
```

### 4.7 Lua 小助手

```cpp
api->LuaNewGlobalTable(L, "MyMod", 0, 4);           // 创建全局表并留在栈顶
api->LuaPushCFunctionField(L, -1, "Foo", &lFoo, "lFoo");
api->LuaPop(L, 1);
```

### 4.8 本地化（完整）

```cpp
void* loc = api->LocCreate();
api->LocLookup(loc, "LOC_YOUR_KEY");
api->LocPushArgument(loc, "arg0");
const char* utf8 = api->LocToUtf8(loc);
api->Log("%s", utf8 ? utf8 : "");
api->LocDestroy(loc);
// 栈缓冲也可用：char buf[CIV6_HOOK_API_LOC_STRING_BYTES]; api->LocalizationLookup(buf, key);
```

### 4.9 常用 GameCore 封装

```cpp
api->SendGameEvent(eventId, a, b);
void* player = api->EditPlayer(playerId);
const char* civ = api->GetCivilizationDescription(player);
uint32_t h = api->MakeHash("QUEST_SEND_TRADE_ROUTE");
```

### 4.10 符号扩展

```cpp
void* fn = api->RequireFunction("GameCore::Utilities::MakeHash"); // 缺失打日志
api->RegisterGameCoreRva("MyAlias", 0x12340);
api->RegisterPushMethodsTarget("GameCore::Lua::IFoo", kHookApiPushNoManager, nullptr);
```

---

## 5. FAQ

Q：需要前置 mod 吗？

A：不需要。每个 DLL mod 都自带薄壳 + runtime，玩家无需再装「空 loader」前置。

Q：为何要将 loader 拆成薄壳 + runtime？

A：多个独立 DLL mod 都会改 `GameCores` 抢同一槽位，游戏最终只加载**其中一个**薄壳。拆开之后：薄壳只负责进槽位并扫描兄弟 mod；真正干活的公共逻辑在 `Civ6DllLoader.runtime.dll`，由薄壳在已启用 mod 里选**版本号最高**的那份启动。这样即使只加载了 A 的薄壳，仍可用 B 带来的更新 runtime，并加载所有启用 mod 的 `Plugins\*.dll`。若不拆分，每次修 loader 都得让玩家换掉「唯一进槽」的那整个 DLL，旧 mod 很难被新 runtime 带动升级。薄壳仅在需要解决重大隐患时更新。

Q：兼容其他未采用这个 loader 架构的 DLL mod 吗？

A：不兼容。凡是同样改 `GameCores` / 占用同一 GameCore 槽位的方案（含官方 Community Extension 等）都会互斥；同时启用时结果未定义，通常后写 `PackageId` 的一方胜出。

Q：多 mod 共存时作者要注意什么？

A：

- **每个**功能 mod 都自带薄壳 + runtime 即可；不必再发「纯 loader」空包。
- 若你的 runtime 较旧，而另一启用 mod 带了更新 runtime，**会自动用对方的新 runtime** 加载你的插件——这是预期行为。
- 插件文件名应全局唯一（不要两个 mod 都叫 `Plugins\Helper.dll`）；同名 basename 只保留先扫到的一份。
- 发布 loader 修复时：把更新的 `Civ6DllLoader.runtime.dll` 放进你的下一版 mod，即可抬高竞选版本；旧 mod 通常不必重发薄壳，除非薄壳更新了。

Q：插件没加载 / 功能没生效，先看哪？

A：

1. `Documents\My Games\Sid Meier's Civilization VI\Logs\Civ6DllLoader_Shell.log` — 薄壳是否起来、runtime 竞选结果。
2. 同目录 `Civ6DllLoader_Runtime.log` — 是否扫到你的 `Plugins\*.dll`、`OnLoad` / `HookAPI_IsCompatible` 日志。
3. 确认 `Config.sql` 的 `PackageId` 与 `.modinfo` 的 `id` 一致，且 `DllPrefix` 与薄壳文件名前缀一致（本文模板为 `GameCore_XP2_Shell`）。
4. 确认编的是 **x64 | Release**，DLL 放在 `Binaries\Win64\Plugins\`，并已导出 C 链接的 `OnLoad`。

Q：`HookAPI_IsCompatible` 失败怎么办？

A：多半是当前竞选到的 runtime 太旧（`structSize` 不够），换用带更高版本 `Civ6DllLoader.runtime.dll` 的 mod 即可。若 loader 做了破坏性 ABI 变更，则需用新的 `HookAPI.h` 重编插件。末尾追加字段通常不必 bump 版本、旧插件仍可跑；只有你用到了新字段时才需要够大的 runtime。

Q：hook / 按地址取函数失效？

A：检查`GetFunctionByAddress`（RVA）和硬编码偏移是否正确。优先改用 `GetFunctionByName` / 符号库友好名；仍对不上时看 Shell/Runtime 日志里的 SizeOfImage 警告，并对照新 DLL 更新 RVA 或向 loader 维护者补符号别名。

Q：`OnUnload` 里要做什么？

A：尽量什么都不做（空实现）。不要依赖 `DLL_PROCESS_DETACH` 做复杂清理；进程退出路径不安全，也容易和宿主卸载顺序打架。

Q：本仓库 recipe 里的 `Civ6Mod/` 能直接当成品 mod 用吗？

A：不能。recipe 示范的是写法；`Civ6Mod/` 只有素材、不含可安装 `.modinfo` / 薄壳 / runtime。发布时对照本文第 2 节目录模板，或复制 `docs/template/` 自行填 GUID 并接上你的 `Plugins\*.dll`。
