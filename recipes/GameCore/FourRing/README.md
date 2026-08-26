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
msbuild FourRing\dll\FourRing.vcxproj /p:Configuration=Release /p:Platform=x64
```

产物：`x64/Release/FourRing.dll`。接到自己的 mod 后看 `Civ6DllLoader_Runtime.log`：

`FourRing: hooked ComputePlotsFromRadius -> always 61`
