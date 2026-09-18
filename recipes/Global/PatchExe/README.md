# PatchExe — 修改 DX11 容量判断的 Global 示例

通过 `HookAPI::Patch` 修改 `ResourceList.AddResource` 的容量判断，演示 exe
补丁、指令校验和恢复。保持原示例行为：将比较阈值从 **8192 降为 256**，
让容量错误路径更容易触发；这不是提高容量的补丁。

## 已核对的指令

参考程序：`CivilizationVI.exe`，版本 `1.0.12.68 (1023995)`，PE timestamp
`0x667C6FC6`，SizeOfImage `0x036D1000`。

SHA-256：`8396ACA228300A47A79A18697A927C8A4526B317E689558DDC24925F1523B99D`。

```text
RVA 0x9407CD: 3D 00 20 00 00    cmp eax, 0x2000
RVA 0x9407CF:       20 -> 01
修改后的指令: 3D 00 01 00 00    cmp eax, 0x100
```

`PatchTargetSignature.h` 保存从 RVA `0x9407C9` 开始的 16 字节完整指令窗口。
`OnLoad` 先检查 ABI、必要接口和 DX11 映像，再通过 `ResolveByRva` 取得映射后的
窗口，逐字节核对后才写入。无法解析、原字节已被修改或指令不匹配时跳过。
局部签名不代表自动支持所有未来版本；更新后仍应核对指令含义。

窗口不包含函数入口，因此可以与 HookExe 的入口 Hook 一起使用。
DX12 的同一 RVA 是不同代码；本示例会明确跳过 DX12，不会使用 DX11 的偏移修改它。

## 构建与部署

在仓库根目录的 x64 Developer PowerShell 中执行：

```powershell
msbuild recipes\Global\PatchExe\dll\PatchExe.vcxproj /p:Configuration=Release /p:Platform=x64
```

VS 2026 使用 v145 时可追加 `/p:PlatformToolset=v145`。Debug/Release 均定义
`CIV6_PLUGIN_GLOBAL`，头文件与当前 Loader 同步。

产物为 `x64/Release/PatchExe.dll`，放到已启用 Mod 的：

```text
Binaries/Win64/Plugins/Global/PatchExe.dll
```

## 验证

完全退出游戏后启动 DX11，`Civ6DllLoader_Loader.log` 应出现：

```text
PatchExe: patched exe anchor RVA 0x9407CF @ ...: 20 -> 01
```

Global 插件在启动时加载，并随进程保持；返回主菜单、读档不应卸载或再次打补丁。
重复 `OnLoad` 会保留第一次的恢复记录。与 HookExe 一起测试时，读档后
HookExe 的计数应继续增长，不应从头开始。

实际达到容量条件时，`Renderer.log` 可能出现 `ResourceList.AddResource()`
的容量提示；它仍打印原来的 8192，因为本示例只修改判断立即数，不修改日志参数。
没有出现容量提示不能单独判定补丁失效。

`OnUnload` 使用 `Unpatch` 恢复原字节；恢复失败会记录 `rollback still pending`
并保留重试状态，Loader 还有按 owner 恢复补丁的清理路径。
普通 GameCore 重载不调用 Global 的 `OnUnload`；关闭进程时也不能以是否出现
该日志判断通过。禁用或替换 Global DLL 后需重启游戏。

启动 DX12 应记录 `DX12 unsupported` 并跳过。当前适配不包含 DX12 的功能补丁。
