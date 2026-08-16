# Civ6DllLoader-Examples

[Civ6DllLoader](https://github.com/sociometry/Civ6DllLoader) 的 DLL mod 写法示范集合。

## Layout

```text
recipes/             各示范（dll + 可能有的 mod 工程）
docs/                作者指南与 mod 模板
```

每个 recipe：

```text
recipes/<Name>/
  README.md
  dll/               VS 插件工程
  Civ6Mod/           可选：mod 工程
```

本仓库**不提供**可直接使用的完整 mod 。自行组装见 `docs/`。

## Recipes

| Recipe | 说明 |
|--------|------|
| `FourRing` | 实战 hook（`ComputePlotsFromRadius`） |

## Build

1. 先构建 **Civ6DllLoader**（薄壳 + runtime）。
2. 然后构建 recipe 的 dll。
3. 产物默认在仓库根 `x64/Release/<Name>.dll`。
4. 把 DLL 与 `Civ6Mod/` 素材接到你自己的 mod（参见 `docs/PLUGIN_GUIDE.md`、`docs/template/`）。

升级 `HookAPI.h`：从 Civ6DllLoader 发行包再复制进各 `recipes/*/dll/`。
