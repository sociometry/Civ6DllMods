# ModConfig

> 中文 | [English](README.en.md)

> 分类：**APIExtension** · 返回 [工程总览](../../../README.md)

给模组作者的开箱即用键值持久化 Lua 接口。

## 为什么需要

模组想保存玩家设置或进度数据，原版**没有任何现成接口**。作者只能自己拼接
存档 / 土路 SQL / 玩家文档目录写文件，跨存档、跨版本极易出错。

## 数据落地

```
Documents\My Games\Sid Meier's Civilization VI\ModConfigs\<名字>.json
```

跟随系统 Known Folder 解析，**OneDrive 重定向也能正确落地**；可用环境变量
`CIV6_MODCONFIG_DIR` 覆盖，便于本地调试。

## Lua 接口

```lua
-- 整份读写
local cfg = ModConfig.Load("MyMod")            -- table（文件不存在时返回空表）
cfg.scale = 1.25
ModConfig.Save("MyMod", cfg)                   -- true / false

-- 单键读写（每次写盘）
ModConfig.Set("MyMod", "scale", 1.25)          -- true / false
local v = ModConfig.Get("MyMod", "scale", 1)   -- 带缺省值
ModConfig.Has("MyMod", "scale")                -- boolean
ModConfig.Delete("MyMod", "scale")             -- boolean
ModConfig.Keys("MyMod")                        -- array of string
ModConfig.Exists("MyMod")                      -- 文件是否存在
ModConfig.GetPath("MyMod")                     -- 调试：看到底写到哪个文件
ModConfig.DeleteFile("MyMod")                  -- 整份删除
ModConfig.Version                              -- "1.0"
```

支持的值类型：`nil` / `boolean` / `number` / `string` / **数组 table**。

> **表的两种形态都支持**：`LuaObjLen > 0` 按数组读（`LuaRawGetI`），否则按对象用 `LuaNext` 遍历哈希
> 部分 —— `{a=1}` 会正常存成 `{"a":1}`。要存不同结构的键值，既可以用嵌套 table，也可以用
> `ModConfig.Set(name, "a/b/c", value)` 逐项写。

## 为什么是 JSON，为什么自己写解析器

- 纯文本、UTF-8、可人读可 diff，玩家和作者都能直接看 / 改；
- 结构（嵌套表、数组）天然对应 Lua table，往返无信息损失；
- 插件必须单 DLL 交付（`/MT` 静态运行库），不能引第三方库 —— 所以自带 `MiniJson.h`
  （约 400 行，RFC 8259 解析 + 序列化，**带深度 / 大小上限**）。

## 文件名规则

`name` 就是 API 第一个参数，**原样（清洗后）当文件名**，不读 `.modinfo` 也不读 mod GUID：

| 传入 `name` | 实际文件 |
| --- | --- |
| `"MyMod"` | `MyMod.json` |
| `"My Awesome Mod"` | `My Awesome Mod.json` |
| `"我的模组"` | `我的模组.json`（UTF-8 原样保留） |
| `"MyMod/v2"` | `MyMod_v2.json` |
| `"  spaced  "` | `spaced.json` |
| `"trailing."` | `trailing.json` |
| `""` / `".."` | `default.json`（兜底） |

清洗规则：

- Windows 保留字符 `< > : " / \ | ? *` 与控制字符 → `_`；
- 去首尾空格与结尾的点；
- 最长 **96 字节**（按 UTF-8 边界截断）；
- 清洗后为空则用 `default`。

**同名即同文件**，请用足够独特的名字；`ModConfig.GetPath(name)` 可确认真实路径。

## 挂载点：`RegisterGlobalScript`

这个入口的语义就是「注册全局可用的接口」—— loader 会把它挂到**每一个脚本环境**
（主菜单 / 对局 gameplay / 对局 UI），所以**只在本工程写一份就够**。

```cpp
void RegisterMainLuaGlobals(void* L) {
    modconfig::SetApi(g_api);
    modconfig::RegisterLuaTable(L);
}
api->RegisterGlobalScript(&RegisterMainLuaGlobals);
```

## 实现要点

### 纯 C++，源码里没有一行 Lua

table → JSON 用 `LuaObjLen` + `LuaRawGetI` 读数组、`LuaNext` 遍历哈希、
`LuaType` / `LuaToNumber` / `LuaToBoolean` / `LuaCheckLString` 读标量；
JSON → table 由 C++ 逐节点压栈构造。

**真实限制**（`ModConfigCore.h` 的 `ReadLuaTable` / `ReadLuaValue`）：

- 嵌套深度上限 **16**：超限的那**一项**返回失败，键值对被跳过，不导致整个表写失败；
- JSON 的对象键必须是 string：字符串键原样用；**整数值**的数字键转成十进制字符串当键名
  （`|d| < 1e15` 且 `d == floor(d)` 才行）；非整数数字键、boolean 键、table 键一律跳过；
- 空表按对象处理（写成 `{}`），不会变成 `[]`。

### 写盘原子性

先写 `<name>.json.tmp`，再 `MoveFileEx(MOVEFILE_REPLACE_EXISTING)` 替换；
失败才回退直接覆盖。**断电 / 崩溃不会留下半个文件**。

### 跨插件同步

读盘前比对 `mtime + size`，未变化直接吃内存缓存。Global 与 GameCore 两个实例
读写同一批文件时，天然看到对方的最新写入。

### 损坏文件自愈

解析失败时把原文件改名成 `.corrupt-1`…`.corrupt-9` 保留，然后从空对象重新开始 ——
**不会因为一个坏文件让 mod 永久起不来**。

## 与 GameCore 侧的关系

两侧各有一份 ModConfig，但**职责不同**：

| 侧 | 挂载点 | 覆盖范围 |
| --- | --- | --- |
| Global（本模块） | `RegisterGlobalScript` | 引擎主 Lua VM，换局不失效 |
| GameCore | `RegisterGameCoreScriptForGamePlay/UI` | 对局的 gameplay / UI 两个脚本环境 |

两者读写**同一批 JSON 文件**，靠 `ModConfigCore` 的 mtime 缓存保持一致。

## 文件结构

| 文件 | 作用 |
| --- | --- |
| `MiniJson.h` | 极简 JSON 容器 / 解析 / 序列化 |
| `ModConfigCore.h` | 文件 IO + Lua 绑定（header-only，两侧共用） |
| `ModConfig.h` / `ModConfig.cpp` | 入口：挂到引擎主 state（进程级，唯一一份） |

## 依赖

**不依赖 asmjit / Zydis** —— 纯 Win32 + 自带 MiniJson。

## 日志

```
[ModConfig] module initialized (main lua state)
[ModConfig] registered; dir=...\ModConfigs
```
