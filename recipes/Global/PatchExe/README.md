# PatchExe — patch 游戏 exe 示例

示范 Global 插件通过 `HookAPI::Patch` 修改 DX11
`CivilizationVI.exe` 的单个字节，并在卸载时通过 `Unpatch` 恢复。

## 补丁

- 目标：`ResourceList.AddResource` 相关逻辑
- RVA：`0x9407CF`
- 新值：`0x01`
- 适用程序：`Win64Steam/CivilizationVI.exe`（非 DX12）

代码使用统一入口
`ResolveByRva(kHookImageExeDx11, 0x9407CF, 1)` 解析地址；若当前是 DX12
或 RVA 越界，接口直接返回空。

## 构建与部署

```powershell
msbuild recipes\Global\PatchExe\dll\PatchExe.vcxproj /p:Configuration=Release /p:Platform=x64
```

产物：

```text
x64/Release/PatchExe.dll
```

部署到：

```text
Binaries/Win64/Plugins/Global/PatchExe.dll
```

## 验证

`Civ6DllLoader_Runtime.log` 应出现：

```text
PatchExe: patched exe RVA 0x9407CF @ ...: .. -> 01
```

随后 `Renderer.log` 应出现：

```text
Error: ResourceList.AddResource(), attempting to add more than %d items.
```

卸载插件时，Runtime 日志应出现恢复记录。

## 注意

该 RVA 仅针对 DX11 的 `CivilizationVI.exe`。示例会拒绝
`CivilizationVI_DX12.exe`，但不会以固定原字节作为版本签名；游戏更新后应重新核对
RVA 和指令含义，再部署补丁。
