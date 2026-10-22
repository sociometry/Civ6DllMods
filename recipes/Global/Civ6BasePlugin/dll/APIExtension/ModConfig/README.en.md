# ModConfig

> English | [中文](README.md)

> Category: **APIExtension** · Back to [project overview](../../../README.md)

Out-of-the-box key-value persistence Lua interface for mod authors.

## Why it is needed

A mod that wants to store player settings or progress data has **no ready-made interface at all** in vanilla. Authors have to roll their own: stuffing data into save files, going through the crude SQL route, or writing files into the player's Documents directory — all of which go wrong very easily across saves and across versions.

## Where the data lands

```
Documents\My Games\Sid Meier's Civilization VI\ModConfigs\<name>.json
```

Resolved through the system Known Folder, so **OneDrive redirection still lands correctly**; the environment variable `CIV6_MODCONFIG_DIR` can override it, which is handy for local debugging.

## Lua interface

```lua
-- whole-document read/write
local cfg = ModConfig.Load("MyMod")            -- table (empty table when the file does not exist)
cfg.scale = 1.25
ModConfig.Save("MyMod", cfg)                   -- true / false

-- single-key read/write (flushes to disk every time)
ModConfig.Set("MyMod", "scale", 1.25)          -- true / false
local v = ModConfig.Get("MyMod", "scale", 1)   -- with a default value
ModConfig.Has("MyMod", "scale")                -- boolean
ModConfig.Delete("MyMod", "scale")             -- boolean
ModConfig.Keys("MyMod")                        -- array of string
ModConfig.Exists("MyMod")                      -- whether the file exists
ModConfig.GetPath("MyMod")                     -- debugging: which file it actually writes to
ModConfig.DeleteFile("MyMod")                  -- delete the whole document
ModConfig.Version                              -- "1.0"
```

Supported value types: `nil` / `boolean` / `number` / `string` / **array tables**.

> **Both table shapes are supported**: when `LuaObjLen > 0` the table is read as an array (`LuaRawGetI`); otherwise it is walked as an object with `LuaNext` over the hash part — so `{a=1}` is stored correctly as `{"a":1}`. To store differently shaped key-value data you can either nest tables or write item by item with `ModConfig.Set(name, "a/b/c", value)`.

## Why JSON, and why a hand-written parser

- Plain text, UTF-8, human-readable and diffable — players and authors can both read and edit it directly;
- The structure (nested tables, arrays) maps naturally onto Lua tables, with no information loss on round-trip;
- A plugin must ship as a single DLL (`/MT` static runtime) and cannot pull in third-party libraries — hence the bundled `MiniJson.h` (about 400 lines, RFC 8259 parsing + serialisation, **with depth and size limits**).

## File name rules

`name` is the API's first argument and is used **as-is (after sanitising) as the file name**; neither `.modinfo` nor the mod GUID is consulted:

| `name` passed in | Actual file |
| --- | --- |
| `"MyMod"` | `MyMod.json` |
| `"My Awesome Mod"` | `My Awesome Mod.json` |
| `"我的模组"` | `我的模组.json` (UTF-8 preserved as-is) |
| `"MyMod/v2"` | `MyMod_v2.json` |
| `"  spaced  "` | `spaced.json` |
| `"trailing."` | `trailing.json` |
| `""` / `".."` | `default.json` (fallback) |

Sanitising rules:

- Windows reserved characters `< > : " / \ | ? *` and control characters → `_`;
- Leading and trailing spaces and trailing dots are stripped;
- At most **96 bytes** (truncated on a UTF-8 boundary);
- Empty after sanitising → `default`.

**Same name means the same file**, so pick a name unique enough; `ModConfig.GetPath(name)` confirms the real path.

## Mount point: `RegisterGlobalScript`

The semantics of this entry point are exactly "register an interface that is globally available" — the loader mounts it into **every script environment** (main menu / match gameplay / match UI), so **writing it once in this project is enough**.

```cpp
void RegisterMainLuaGlobals(void* L) {
    modconfig::SetApi(g_api);
    modconfig::RegisterLuaTable(L);
}
api->RegisterGlobalScript(&RegisterMainLuaGlobals);
```

## Implementation notes

### Pure C++, not a single line of Lua in the source

table → JSON uses `LuaObjLen` + `LuaRawGetI` for arrays, `LuaNext` for the hash part, and `LuaType` / `LuaToNumber` / `LuaToBoolean` / `LuaCheckLString` for scalars; JSON → table is built node by node in C++ and pushed onto the stack.

**Real limitations** (`ReadLuaTable` / `ReadLuaValue` in `ModConfigCore.h`):

- Maximum nesting depth **16**: the **entry** that exceeds it fails, the key-value pair is skipped, and it does not fail the whole document write;
- JSON object keys must be strings: string keys are used as-is; **integer-valued** numeric keys become their decimal string form (`|d| < 1e15` and `d == floor(d)` required); non-integer numeric keys, boolean keys and table keys are skipped;
- An empty table is treated as an object (written as `{}`) and never becomes `[]`.

### Atomic writes

Write `<name>.json.tmp` first, then replace with `MoveFileEx(MOVEFILE_REPLACE_EXISTING)`; only fall back to a direct overwrite if that fails. **A power cut or a crash never leaves half a file behind.**

### Cross-plugin synchronisation

`mtime + size` are compared before reading from disk, and an unchanged file is served straight from the in-memory cache. When the Global and GameCore instances read and write the same batch of files, each naturally sees the other's latest writes.

### Self-healing from corrupt files

On a parse failure the original file is renamed to `.corrupt-1`…`.corrupt-9` for safekeeping and the module restarts from an empty object — **a single bad file never keeps the mod from ever starting again**.

## Relationship to the GameCore side

Both sides have their own ModConfig, but with **different responsibilities**:

| Side | Mount point | Coverage |
| --- | --- | --- |
| Global (this module) | `RegisterGlobalScript` | the engine's main Lua VM; survives match changes |
| GameCore | `RegisterGameCoreScriptForGamePlay/UI` | the match's gameplay / UI script environments |

Both read and write **the same batch of JSON files**, kept consistent by `ModConfigCore`'s mtime cache.

## File layout

| File | Purpose |
| --- | --- |
| `MiniJson.h` | minimal JSON container / parsing / serialisation |
| `ModConfigCore.h` | file IO + Lua binding (header-only, shared by both sides) |
| `ModConfig.h` / `ModConfig.cpp` | entry point: attached to the engine main state (process-level, a single instance) |

## Dependencies

**No dependency on asmjit / Zydis** — pure Win32 plus the bundled MiniJson.

## Logging

```
[ModConfig] module initialized (main lua state)
[ModConfig] registered; dir=...\ModConfigs
```
