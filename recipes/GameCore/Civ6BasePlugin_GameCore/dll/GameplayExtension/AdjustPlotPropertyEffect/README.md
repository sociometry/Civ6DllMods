# AdjustPlotPropertyEffect

> 中文 | [English](README.en.md)

往 GameEffects 注册表注入 `EFFECT_ADJUST_PLOT_PROPERTY`（`GameplayExtension` 分类下的示例模块）。

## 当前行为

`AdjustPlotProperty.cpp` 的 `AdjustPlotPropertyEffect_Init` 注册该 effect（由插件入口 `dllmain.cpp` 的 `OnLoad` 调用）：

1. 解析 GameCore 符号
2. 克隆 `EffectFactory<AdjustUnitProperty>` / `AdjustUnitProperty` 虚表
3. 改写 factory `Create` / `GetTypeName`，以及 UnitEffect 的 Apply/Remove
4. 把 factory 挂进 `Registry<IModifierEffectFactory>::GetTypes()`
5. Apply/Remove：经 `IPlotInterface` → 地块动态属性 ±delta

配套 SQL 在 [Civ6Mod/Data/Gameplay.sql](../../../Civ6Mod/Data/Gameplay.sql) 的 `B.` 段（与两个 Requirement 的示范数据在同一份文件里）。

## 构建

```powershell
& MSBuild.exe recipes\GameCore\Civ6BasePlugin_GameCore\dll\Civ6BasePlugin_GameCore.vcxproj /t:Rebuild /m /p:Configuration=Release /p:Platform=x64
```

产物：`x64\Release\Civ6BasePlugin_GameCore.dll`（本模块已并进这个插件）。

## 文件

- `AdjustPlotProperty.cpp`：实现
- `AdjustPlotPropertyEffect.h`：`*_Init` / `*_Shutdown` 声明
- 配套 SQL：`Civ6Mod/Data/Gameplay.sql` 的 `B.` 段（不在本目录）

## 当前 Loader 兼容性与重载验证

- `dll/HookAPI.h` 与当前 Civ6DllLoader 的 `common/HookAPI.h` 同步；升级时复制整份头文件并重新编译。
- Debug/Release 均定义 `CIV6_PLUGIN_GAMECORE`，由头文件生成 `Civ6PluginKind` 导出；`OnLoad` 首先检查 ABI。
- 从仓库根执行上面的构建命令。VS 2026 本机只有 v145 时可追加 `/p:PlatformToolset=v145`；工程默认保留 v143。
- 将产物放在已启用 Mod 的 `Binaries/Win64/Plugins/GameCore/`，配合本次更新的 Loader 测试 XP2。
- 同一游戏进程内反复执行“进对局 → 触发功能 → 保存 → 返回主菜单 → 读档 → 再次触发功能”。
- 检查 `Civ6DllLoader_Loader.log` 中的插件加载、卸载排队、引擎卸载后释放、再次加载，以及本插件的成功日志；编译通过不能代替游戏内验证。
- 当前映像 RVA 的备用解析走 `ResolveByRvaEx(..., kSectionImage)`；旧 RVA 才走 `ResolveByRva` 的坐标修正。
- 克隆虚表保留 `vtable[-1]` 的 RTTI；factory 使用游戏分配器，容量不足时不分配。
- 验证 effect 应用与移除时地块属性分别增加和还原；读档后再次执行。
