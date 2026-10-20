# Civ6BasePlugin（Global 侧）

Civilization VI 的「基础插件集」Global 侧，跑在 [Civ6DllLoader] runtime 的 HookAPI 插件架构上。

- **插件类别**：Global（宏 `CIV6_PLUGIN_GLOBAL`），入口 `OnLoad(HookContext* ctx)`
- **生命周期**：随进程常驻，主菜单与对局期间都在
- **部署位置**：`<Mod>/Binaries/Win64/Plugins/Global/Civ6BasePlugin_Global.dll`
- **支持映像**：DX11 `CivilizationVI.exe` 与 DX12 `CivilizationVI_DX12.exe`，运行期自动判别

姊妹工程：[GameCore 侧](../../GameCore/Civ6BasePlugin_GameCore/README.md)（进入对局才加载，退局即卸载）。

---

## 模块清单

每个模块都有一份独立 README，讲清楚它解决什么问题、怎么实现、踩过哪些坑。

### BugFix —— 修原版缺陷

| 模块 | 解决什么问题 |
| --- | --- |
| [UnlockModelLimit](dll/BugFix/UnlockModelLimit/README.md) | 引擎的模型 / 材质数量上限：原版资源一多就报错甚至渲染失败，装后可支撑更大的模组规模 |

### APIExtension —— 补 Lua 接口

| 模块 | 补什么接口 |
| --- | --- |
| [ModConfig](dll/APIExtension/ModConfig/README.md) | 一组开箱即用的键值持久化 Lua API，用来保存玩家设置与进度数据 |

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
    ├── BugFix/UnlockModelLimit/             + README.md
    │   ├── UnlockModelLimit.h               # 入口声明
    │   ├── UnlockModelLimit_Common.h        # 共享工具（Patch / 解码 / hook 安装 / 站点解析）
    │   ├── UnlockModelLimit.cpp             # DX11 分支
    │   └── UnlockModelLimit_Dx12.cpp        # DX12 分支
    └── APIExtension/ModConfig/              + README.md
        ├── MiniJson.h                       # 极简 JSON 容器 / 解析 / 序列化
        ├── ModConfigCore.h                  # 文件 IO + Lua 绑定（header-only）
        ├── ModConfig.h
        └── ModConfig.cpp                    # 入口：挂到引擎主 state
```

---

## 构建

```powershell
& 'E:\VisualStudio\Products\MSBuild\Current\Bin\amd64\MSBuild.exe' `
  'recipes\Global\Civ6BasePlugin\dll\Civ6BasePlugin_Global.vcxproj' `
  /p:Configuration=Release /p:Platform=x64
```

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

runtime 日志（`%LOCALAPPDATA%\Firaxis Games\Sid Meier's Civilization VI\Logs\Civ6DllLoader_Loader.log`）
应出现：

```
UnlockModelLimit: exe image = DX12
[DX12] hooks installed: %d/2
UnlockModelLimit[DX12]: installed (%zu patches)
[ModConfig] registered; dir=...\ModConfigs
```

> `%d` / `%zu` 是格式串里的运行时数值，不是固定数字；站点集合随 exe 版本变化。
> DX11 分支的对应行是 `UnlockModelLimit: installed (%zu patches, %d/4 hooks)`。

任何一条缺失、或出现 `redirects incomplete`，说明该 exe 版本的站点形态变了，补丁会整体跳过
（功能不生效，但不改内存）。

> 各模块的具体验证方法与日志样例见其 README。

---

## 依赖

第三方库已 vendored 在 `dll/include` 与 `dll/lib`：

| 库 | 版本 | 判据 |
| --- | --- | --- |
| asmjit | **1.17** | `include/asmjit/core/api-config.h` 的 `ASMJIT_ABI_NAMESPACE v1_17`；`lib/asmjit.lib` 导出符号带 `@v1_17@` |
| Zydis | **4.1.0** | `include/Zydis/Zydis.h` 的 `ZYDIS_VERSION 0x0004000100000000` |
| Zycore | **1.5.2** | `include/Zycore/Zycore.h` 的 `ZYCORE_VERSION 0x0001000500020000` |

（ModConfig 不依赖它们，纯 Win32 + 自带 MiniJson。）
升级这几个库时三处版本表要一起改，并重新核对 `ASMJIT_STATIC` / `ZYDIS_STATIC_BUILD` 两个宏。

---

## 注意事项

- **版本绑定**：站点 RVA 针对当前 exe 构建。游戏更新后优先看 AOB 兜底是否生效；
  日志出现「改走 AOB」说明主路径已漂移，应更新 RVA 与 AOB。
- **内存补丁**：只改进程内存，不动磁盘文件。
- **扩展数组**：位于 imagebase ±2GB 内，与引擎原数组的 disp32 寻址方式一致。
- **`/utf-8` 必需**：仓库 `.editorconfig` 约定 UTF-8 无 BOM，MSVC 默认按系统代码页解析
  中文注释会报错。

---

## 参考

- 仓库总览：[`README.md`](../../../README.md)
- 插件开发规范：[`docs/PLUGIN_GUIDE.md`](../../../docs/PLUGIN_GUIDE.md)
- 补丁逻辑源自 **DllProxy**（`version.dll` 代理方案），后改造为 HookAPI 插件形态

[Civ6DllLoader]: https://github.com/sociometry/Civ6DllLoader
