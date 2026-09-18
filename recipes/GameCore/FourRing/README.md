# FourRing — hook 示例：四环地块数

把 `GameCore::City::CityHexspace::ComputePlotsFromRadius` 固定返回 **61**（四环格子总数），验证「按名取符号 + InstallHook」。

## 公式对照

| 半径 | 格子数 |
|------|--------|
| 0 | 1 |
| 1 | 7 |
| 2 | 19 |
| 3 | 37 |
| **4** | **61** |

原函数：`f(0)=1; f(n)=f(n-1)+6*n`。恒返回 61 ≈ 按四环配额放宽。

## 构建

```powershell
msbuild recipes\GameCore\FourRing\dll\FourRing.vcxproj /p:Configuration=Release /p:Platform=x64
```

产物：`x64/Release/FourRing.dll`。接到自己的 mod 后看 `Civ6DllLoader_Loader.log`：

`FourRing: hooked ComputePlotsFromRadius -> always 61`

## 当前 Loader 兼容性与重载验证

- `dll/HookAPI.h` 与当前 Civ6DllLoader 的 `common/HookAPI.h` 同步；升级时复制整份头文件并重新编译。
- Debug/Release 均定义 `CIV6_PLUGIN_GAMECORE`，由头文件生成 `Civ6PluginKind` 导出；`OnLoad` 首先检查 ABI。
- 从仓库根执行上面的构建命令。VS 2026 本机只有 v145 时可追加 `/p:PlatformToolset=v145`；工程默认保留 v143。
- 将产物放在已启用 Mod 的 `Binaries/Win64/Plugins/GameCore/`，配合本次更新的 Loader 测试 XP2。
- 同一游戏进程内反复执行“进对局 → 触发功能 → 保存 → 返回主菜单 → 读档 → 再次触发功能”。
- 检查 `Civ6DllLoader_Loader.log` 中的插件加载、卸载排队、引擎卸载后释放、再次加载，以及本插件的成功日志；编译通过不能代替游戏内验证。
