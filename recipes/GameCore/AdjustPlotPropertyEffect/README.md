# AdjustPlotPropertyEffect

在 runtime 中注入 `EFFECT_ADJUST_PLOT_PROPERTY` 的脚手架示范。

## 当前行为

`AdjustPlotProperty.cpp` 在 `OnLoad` 注册该 effect：

1. 解析 GameCore 符号
2. 克隆 `EffectFactory<AdjustUnitProperty>` / `AdjustUnitProperty` 虚表
3. 改写 factory `Create` / `GetTypeName`，以及 UnitEffect 的 Apply/Remove
4. 把 factory 挂进 `Registry<IModifierEffectFactory>::GetTypes()`
5. Apply/Remove：经 `IPlotInterface` → 地块动态属性 ±delta

配套 SQL/Lua 在 `Civ6Mod/`（无 `.modinfo`）。

## 构建

```powershell
msbuild recipes\GameCore\AdjustPlotPropertyEffect\dll\AdjustPlotPropertyEffect.vcxproj /p:Configuration=Release /p:Platform=x64
```

产物：`x64/Release/AdjustPlotPropertyEffect.dll`。组装完整 mod 见 `docs/template/`。

## 文件

- `dll/AdjustPlotProperty.cpp` / `.h`：实现
- `dll/dllmain.cpp`：入口
- `Civ6Mod/`：`Data` / `Script` 素材

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
