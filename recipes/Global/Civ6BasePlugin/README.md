# Civ6BasePlugin（Global 侧）

Civilization VI 的「基础插件集」Global 侧，跑在 [Civ6DllLoader] runtime 的 HookAPI 插件架构上。

- **插件类别**：Global（宏 `CIV6_PLUGIN_GLOBAL`），入口 `OnLoad(HookContext* ctx)`
- **生命周期**：随进程常驻，主菜单与对局期间都在
- **部署位置**：`<Mod>/Binaries/Win64/Plugins/Global/Civ6BasePlugin_Global.dll`
- **支持映像**：DX11 `CivilizationVI.exe` 与 DX12 `CivilizationVI_DX12.exe`，运行期自动判别

姊妹工程：[GameCore 侧](../../GameCore/Civ6BasePlugin_GameCore/README.md)（进入对局才加载，退局即卸载）。

---

## 模块清单

| 模块 | 分类 | 功能 |
| --- | --- | --- |
| **UnlockModelLimit** | BugFix | 解除渲染资源上限（IB/VB 8192→16384，SPI 16384→32768），只改内存不改磁盘 |
| **ModConfig** | APIExtension | 全局 Lua 键值持久化接口，数据落 `Documents\\My Games\\Sid Meier's Civilization VI\\ModConfigs\\<名字>.json` |

---

## 1. UnlockModelLimit —— 解除渲染资源上限

| 资源对象 | 原始上限 | 扩展后 |
| --- | --- | --- |
| IB / VB（Index / Vertex Buffer） | 8192 | 16384 |
| SPI（ShaderPackageInstance） | 16384 | 32768 |

超出原上限的句柄改写进插件自己分配的扩展数组，引擎原有的静态数组保持不动。

### 实现要点

1. **扩展数组**：`VirtualAlloc` 在 imagebase ±2GB 内分配（保证 disp32 可达）。每张表的结构是 `RefCount / records / ExtraData`，按完整 u16 句柄空间（65536 项）预留。
2. **重定向**：把引擎里读写这些数组的指令改指到扩展数组。
   - `lea rcx, [rip+disp32]` 形态只改 disp32；
   - `[base+idx*4+80h]` 这类索引访问用 asmjit 重新编码整条指令；
   - 站点数量随 exe 不同（DX12 当前 58 处）。
3. **hook**（每个 exe 3 个）：
   - `AddResource` / `Release` 的 refcount 指令 —— 这两个函数被 IB/VB 共用，目标数组由运行时 `this` 决定，单条 disp32 表达不了；
   - `IBVB::Shutdown` 全表清理 —— 整函数替换：按 `rsi` 分流到对应扩展数组，并自实现 `m_DeleteList` 扩容。
4. **上限常量**：最后才改那 6 个立即数字节。前置重定向必须全部成功，否则整组跳过（fail-safe）。

### 兼容性设计

- **全局对象不写死地址**：`m_IBList` / `m_VBList` / `m_SPIList` / `m_ComputeSPIList` 用 `ResolveDataByAob` 从引用它们的 `lea` 指令里现解 disp32。
- **站点走 `HookInfo`**（RVA 主路径 + AOB 兜底）：RVA 落点字节不符时自动改走 AOB，两者都不符则放弃安装。
- **每个字节补丁都校验**原指令形态与期望立即数，不符即拒绝写入。
- 任一环节失败即整体 fail-safe：不装 hook、不打补丁、不改上限常量，游戏保持原样。

### 两个 asmjit 陷阱（实测）

写 trampoline / 重编码指令时踩过，记下来避免重复：

| 写法 | 实际生成 | 说明 |
| --- | --- | --- |
| `a.jmp((uint64_t)addr)` | `FF 25 02 00 00 00` + 内联指针（16 字节） | **间接跳转**——会读目标处的 8 字节当指针再跳，不是"跳到该地址" |
| `a.jmp(x86::ptr(addr))` | `RelocOffsetOutOfRange` | 直接报错，不可用 |
| `a.jmp(Label)` | `E9 xx xx xx xx` | 能生成 `E9 rel32`，但 Label 只能指向同一个 CodeHolder 内部 |
| `a.embedUInt8(0xE9); a.embedInt32(rel)` | `E9 ...` | **可行**：asmjit 负责编码，目标地址延后回填 |

另外 **asmjit 对非法操作数只写进 emitter 的局部 `err`**，`rt.add()` 仍返回 `kErrorOk`，
那条指令会被静默丢弃 —— 所以每条 emit 都必须自己检查（见 `UnlockModelLimit_Common.h` 的 `ASMJIT_EMIT`）。

---

## 2. ModConfig —— 全局 Lua 配置接口

给 mod 作者的开箱即用键值持久化接口，数据落在：

```
Documents\My Games\Sid Meier's Civilization VI\ModConfigs\<名字>.json
```

（跟随系统 Known Folder 解析，OneDrive 重定向也能正确落地；可用环境变量 `CIV6_MODCONFIG_DIR` 覆盖，便于本地调试。）

### 为什么是 JSON，为什么自己写解析器

- 纯文本、UTF-8、可人读可 diff，玩家和作者都能直接看/改；
- 结构（嵌套表、数组）天然对应 Lua table，往返无信息损失；
- 插件必须单 DLL 交付（`/MT` 静态运行库），不能引第三方库 —— 所以自带 `MiniJson.h`
  （约 400 行，RFC 8259 解析 + 序列化，带深度/大小上限）。

### Lua 接口

```lua
-- 整份读写
local cfg = ModConfig.Load("MyMod")            -- table（文件不存在时返回空表）
cfg.scale = 1.25
ModConfig.Save("MyMod", cfg)                   -- true / false

-- 单键读写（每次写盘）
ModConfig.Set("MyMod", "scale", 1.25)          -- true / false
local v = ModConfig.Get("MyMod", "scale", 1)   -- 带缺省值
ModConfig.Has("MyMod", "scale")                -- boolean
ModConfig.Delete("MyMod", "scale")             -- boolean
ModConfig.Keys("MyMod")                        -- array of string
ModConfig.Exists("MyMod")                      -- 文件是否存在
ModConfig.GetPath("MyMod")                     -- 调试：看到底写到哪个文件
ModConfig.DeleteFile("MyMod")                  -- 整份删除
ModConfig.Version                              -- "1.0"
```

支持的值类型：`nil` / `boolean` / `number` / `string` / **数组 table**。

> **哈希 table（`{a=1}`）不支持**：HookAPI 没暴露遍历原语，C++ 侧读不了哈希部分，
> 这种表写出去是 `{}`。要存多个键请用 `ModConfig.Set(name, "key", value)` 逐个写。

### 文件名规则

`name` 就是 API 第一个参数，**原样（清洗后）当文件名**，Loader 不读 `.modinfo` 也不读 mod GUID：

| 传入 `name` | 实际文件 |
| --- | --- |
| `"MyMod"` | `MyMod.json` |
| `"My Awesome Mod"` | `My Awesome Mod.json` |
| `"我的模组"` | `我的模组.json`（UTF-8 原样保留） |
| `"MyMod/v2"` | `MyMod_v2.json` |
| `"  spaced  "` | `spaced.json` |
| `"trailing."` | `trailing.json` |
| `""` / `".."` | `default.json`（兜底） |

清洗：Windows 保留字符 `< > : " / \ | ? *` 与控制字符 → `_`；去首尾空格与结尾的点；
最长 96 字节（按 UTF-8 边界截断）；清洗后为空则用 `default`。
**同名即同文件**，请用足够独特的名字；`ModConfig.GetPath(name)` 可确认真实路径。

### 实现要点

1. **注入点**：`RegisterGlobalScript`。这个入口的语义就是"注册全局可用的接口"——loader 会把
   它挂到每一个脚本环境（主菜单 / 对局 gameplay / 对局 UI），所以**只在本工程写一份就够**，
   GameCore 侧不需要重复实现。
2. **纯 C++，源码里没有一行 Lua**：table → JSON 用 `LuaObjLen` + `LuaRawGetI` 读数组、
   `LuaNext` 遍历哈希、`LuaType` / `LuaToNumber` / `LuaToBoolean` / `LuaCheckLString` 读标量；
   JSON → table 由 C++ 逐节点压栈构造。
3. **写盘原子性**：先写 `<name>.json.tmp`，再 `MoveFileEx(MOVEFILE_REPLACE_EXISTING)` 替换；
   失败才回退直接覆盖。断电/崩溃不会留下半个文件。
4. **跨插件同步**：读盘前比对 `mtime + size`，未变化直接吃内存缓存；Global 与 GameCore
   两个实例读写同一批文件时天然看到对方的最新写入。
5. **损坏文件自愈**：解析失败时把原文件改名成 `.corrupt-1`…`.corrupt-9` 保留，然后从空对象
   重新开始 —— 不会因为一个坏文件让 mod 永久起不来。
6. **文件名清洗**：见上表。

### 已验证

- `tools/minijson_test`：JSON 往返、转义、`\uXXXX`、深嵌套保护、错误输入 —— 全绿；
- `tools/modconfig_test`：真实文件系统下的写入/读回/损坏自愈/原子写残留 —— 全绿；
- `tools/modconfig_lua_test_run.lua`：用 Lua 5.1 跑编码器与门面转发 —— 全绿。

---

## 目录结构

```
recipes/Global/Civ6BasePlugin/
├── README.md
└── dll/
    ├── Civ6BasePlugin_Global.vcxproj
    ├── Civ6BasePlugin_Global.vcxproj.filters
    ├── HookAPI.h
    ├── framework.h / pch.h / pch.cpp
    ├── dllmain.cpp                          # OnLoad：ABI 防御 + RequireAll + 各模块 Init
    ├── include/asmjit/                      # vendored asmjit（源码头文件）
    ├── include/Zydis/ , include/Zycore/     # vendored 反汇编库
    ├── lib/asmjit.lib , Zydis.lib , Zycore.lib
    ├── BugFix/UnlockModelLimit/
    │   ├── UnlockModelLimit.h               # 入口声明
    │   ├── UnlockModelLimit_Common.h        # 共享工具（Patch / 解码 / hook 安装 / 站点解析）
    │   ├── UnlockModelLimit.cpp             # DX11 分支
    │   └── UnlockModelLimit_Dx12.cpp        # DX12 分支
    └── APIExtension/ModConfig/
        ├── MiniJson.h                       # 极简 JSON 容器/解析/序列化
        ├── ModConfigCore.h                  # 文件 IO + Lua 绑定（header-only）
        ├── ModConfig.h
        └── ModConfig.cpp                    # 入口：挂到引擎主 state（进程级，唯一一份）
```

---

## 构建

```powershell
& 'E:\VisualStudio\Products\MSBuild\Current\Bin\amd64\MSBuild.exe' `
  'recipes\Global\Civ6BasePlugin\dll\Civ6BasePlugin_Global.vcxproj' `
  /p:Configuration=Release /p:Platform=x64
```

或直接跑仓库脚本：`bash tools/build_global.sh`。

产物：`x64/Release/Civ6BasePlugin_Global.dll`。

工程配置（Release | x64）：`CIV6_PLUGIN_GLOBAL`、`ASMJIT_STATIC`、`ZYDIS_STATIC_BUILD`、
C++20、`/MT`（与 vendored 的 `*.lib` 一致）、`/utf-8`。

---

## 部署

```
<Mod>/Binaries/Win64/Plugins/Global/Civ6BasePlugin_Global.dll
```

---

## 日志

runtime 日志（`%LOCALAPPDATA%\Firaxis Games\Sid Meier's Civilization VI\Logs\Civ6DllLoader_Loader.log`）应出现：

```
UnlockModelLimit: exe image = DX12
[DX12] redirects: 58 ok, 0 failed
[DX12] installed (60 patches)
[ModConfig] registered; dir=...\ModConfigs
```

任何一条缺失、或出现 `redirects incomplete`，说明该 exe 版本的站点形态变了，补丁会整体跳过
（功能不生效，但不改内存）。

---

## 依赖

第三方库已 vendored 在 `dll/include` 与 `dll/lib`：asmjit 93、Zydis 28、Zycore 22。
（ModConfig 不依赖它们，纯 Win32 + 自带 MiniJson。）

---

## 注意事项

- **版本绑定**：站点 RVA 针对当前 exe 构建。游戏更新后优先看 AOB 兜底是否生效；
  日志出现"改走 AOB"说明主路径已漂移，应更新 RVA 与 AOB。
- **内存补丁**：只改进程内存，不动磁盘文件。
- **扩展数组**：位于 imagebase ±2GB 内，与引擎原数组的 disp32 寻址方式一致。
- **`/utf-8` 必需**：仓库 `.editorconfig` 约定 UTF-8 无 BOM，MSVC 默认按系统代码页解析中文注释会报错。

---

## 参考

- 仓库总览：[`README.md`](../../../README.md)
- 插件开发规范：[`docs/PLUGIN_GUIDE.md`](../../../docs/PLUGIN_GUIDE.md)
- ModConfig 详解：[`docs/MODCONFIG.md`](../../../docs/MODCONFIG.md)
- 补丁逻辑源自 **DllProxy**（`version.dll` 代理方案），后改造为 HookAPI 插件形态

[Civ6DllLoader]: https://github.com/sociometry/Civ6DllLoader