# HookExe — Global exe hook 示例

通过 `InstallHook` 观察 DX11 `CivilizationVI.exe` 中 RVA `0x940770` 的
`ResourceList.AddResource`：调用原函数，原样返回结果，并累计调用次数。

## 当前 Loader 适配

- 使用当前 `HookAPI.h`，Debug/Release 都定义 `CIV6_PLUGIN_GLOBAL`，自动导出 `Civ6PluginKind`。
- `OnLoad` 先检查 ABI，再检查 DX11 映像；DX12 跳过。RVA 通过 `ResolveByRva` 做 pdata 坐标修正。
- 重复调用 `OnLoad` 不重复挂钩，也不清零计数。安装失败可以重试。
- 计数只通过日志观察。Global 插件不注册 GameCore Lua 全局表，因此不再提供 `HookExe.GetCallCount()`。
- EXE hook 随 Global 插件常驻，GameCore 进退局、读档不会卸载它。
- `OnUnload` 只记录计数；钩子清理由 Loader 负责，插件不会提前清空在途调用可能使用的 original。

## 构建和部署

在仓库根执行：

```powershell
msbuild recipes\Global\HookExe\dll\HookExe.vcxproj /p:Configuration=Release /p:Platform=x64
```

本机只安装 VS 2026 v145 时追加 `/p:PlatformToolset=v145`；工程默认仍为 v143。
产物 `x64\Release\HookExe.dll` 放入启用 Mod 的
`Binaries\Win64\Plugins\Global\`。Global 插件启动时扫描，改变启用状态后重启游戏。

## 验证

查看 `%LocalAppData%\Firaxis Games\Sid Meier's Civilization VI\Logs\Civ6DllLoader_Loader.log`：

```text
HookExe: hooked exe AddResource @ ... (anchor RVA 0x940770)
HookExe: AddResource call #1 list=... -> id=...
HookExe: AddResource call #200 list=... -> id=...
```

首调和每 200 次调用记录一次。保持同一游戏进程，进行“主菜单 → 进局 → 返回主菜单 → 读档”循环，
确认计数继续累计，安装成功日志不因 GameCore 重载而重复出现。DX12 应记录跳过，不能安装此钩。

当前入口检查仅排除空字节、断点和 NOP，并不是完整的函数签名。游戏更新或其他 Mod 修改此函数后，
仍须核对地址和函数 ABI。这里的 RVA 和函数说明沿用原示例，编译及模拟 API 自检不替代游戏实测。
