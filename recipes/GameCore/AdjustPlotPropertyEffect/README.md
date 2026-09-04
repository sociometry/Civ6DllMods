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
msbuild AdjustPlotPropertyEffect\dll\AdjustPlotPropertyEffect.vcxproj /p:Configuration=Release /p:Platform=x64
```

产物：`x64/Release/AdjustPlotPropertyEffect.dll`。组装完整 mod 见 `docs/template/`。

## 文件

- `dll/AdjustPlotProperty.cpp` / `.h`：实现
- `dll/dllmain.cpp`：入口
- `Civ6Mod/`：`Data` / `Script` 素材
