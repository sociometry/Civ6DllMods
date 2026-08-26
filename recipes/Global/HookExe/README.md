# HookExe — hook 游戏 exe 示例

示范如何用 **Global 插件** 对 `CivilizationVI.exe` 内函数做 `InstallHook`（透传 + 计数日志）。

对照：`FourRing` 是 hook **GameCore DLL** 内符号；本例是 hook **exe**。

## 目标函数

`sub_140940770` @ RVA **`0x940770`**

签名大致为：

```cpp
__int64 __fastcall AddResource(volatile int32_t* list);
```

## 教什么

| 要点 | 说明 |
|------|------|
| 部署目录 | 必须放进 `Plugins\Global\`（阶段 A；exe 已映射） |
| 取地址 | `ResolveByRva(kHookImageExeDx11, RVA, span)` |
| 生命周期 | exe 域钩跨进退局保留，不必 `RegisterGameCoreTeardown` |
| 禁区 | 勿抢 Loader 已 hook 的 `LoadDLL` / `UnloadDLL` / `lEnableMod` / `lDisableMod` |

## 构建

```powershell
msbuild recipes\Global\HookExe\dll\HookExe.vcxproj /p:Configuration=Release /p:Platform=x64
```

产物：`x64\Release\HookExe.dll` → 接到 mod 的：

```text
Binaries/Win64/Plugins/Global/HookExe.dll
```

## 验证

1. `Civ6DllLoader_Runtime.log`：

```text
HookExe: hooked exe AddResource @ ... (RVA 0x940770)
HookExe: AddResource call #1 list=... -> id=...
```

（首调 + 每 200 次打一次。）

2. Gameplay Lua：

```lua
print(HookExe.GetCallCount())
```

进主菜单 / 进局后数字应增长。

## 注意：仅适用于非 DX12 的 `CivilizationVI.exe`

本例 RVA 相对 **Win64Steam/CivilizationVI.exe**。若 Steam 启动的是 `CivilizationVI_DX12.exe`，同一 RVA 落在错误代码上：

- Loader 会报 `LoadDLL signature mismatch`（阶段 B 全部跳过）
- `ResolveByRva(kHookImageExeDx11, ...)` 会因映像 ID 不匹配而返回空

请用非 DX12 启动，或自行针对 DX12 重找 RVA。
