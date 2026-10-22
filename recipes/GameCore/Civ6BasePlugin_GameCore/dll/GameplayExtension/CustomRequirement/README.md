# CustomRequirement

> 中文 | [English](README.en.md)

为文明6风云变幻增加两个自定义 **Requirement**（`GameplayExtension` 分类下的示例模块）：

| 源码 | 负责内容 | 注册后安装的 hook |
|---|---|---|
| [PlayerPropertyMatches.cpp](PlayerPropertyMatches.cpp) | 玩家属性阈值；地址、参数解析、工厂、判断、弱引用和刷新 | PlotProperty OnInitialize / Test、玩家属性 Dispatch、Lua SetProperty（4 个） |
| [PlotRouteTypeMatches.cpp](PlotRouteTypeMatches.cpp) | 道路类型；地址、参数解析、工厂、判断、弱引用和刷新 | Terrain TestPlot、PlotRequirement Test、SetRouteType（3 个） |
| [CustomRequirement.h](CustomRequirement.h) | 两个 `*_Init` / `*_Shutdown` 的声明 | 无 |

调用点在插件入口 [dllmain.cpp](../../dllmain.cpp)：`OnLoad` 依次调 `CustomRequirement_InitPlayerProperty` /
`CustomRequirement_InitPlotRoute`，`OnUnload` 调对应的两个 `*_Shutdown`。任一模块失败不影响另一个
（各自装钩、各自记日志）。

每个 cpp 都有自己的匿名命名空间和状态。地址解析、shared pointer、Registry 等辅助代码有意重复，方便单独阅读和复制；两个模块不互相调用，也不再依赖一个集中实现文件。

## 参数与当前烟测 SQL

| Requirement | 参数 |
|---|---|
| `REQUIREMENT_PLAYER_PROPERTY_MATCHES` | `PropertyName`、`PropertyMinimum`（有符号 32 位阈值） |
| `REQUIREMENT_PLOT_ROUTE_TYPE_MATCHES` | `RouteType`（例如 `ROUTE_ANCIENT_ROAD`） |

配套 SQL 在 [Civ6Mod/Data/Gameplay.sql](../../../Civ6Mod/Data/Gameplay.sql)（本工程只维护一份，Requirement 与 Effect 的示范数据都在里面）。
属性 Modifier 挂在 `TRAIT_LEADER_MAJOR_CIV`，阈值为 1 和 2，各加 1 次工人次数；道路 Modifier 挂在农田上，
远古道路匹配时加 10 金币（旧 ID 叫 `MODIFIER_EXAMPLE_ROAD_FOOD` 但实际参数是 `YIELD_GOLD`，已改名为
`MODIFIER_EXAMPLE_ROAD_GOLD`）。

## 构建与验证

在仓库根目录执行：

```powershell
& MSBuild.exe recipes\GameCore\Civ6BasePlugin_GameCore\dll\Civ6BasePlugin_GameCore.vcxproj /t:Rebuild /m /p:Configuration=Release /p:Platform=x64
```

产物：`x64\Release\Civ6BasePlugin_GameCore.dll`（本模块已并进这个插件，不再单独出 DLL）。

## 新增 Requirement 通用指南

本章总结如何新增一个自定义 Requirement。目标不是复制某一个类，而是掌握所有 Requirement 都共有的连接方式：

```text
SQL 定义
  → Requirement registry 根据类型名寻找 factory
  → factory::Create 创建 native requirement 对象
  → OnInitialize 读取参数、保存上下文
  → Test / TestPlot 计算当前状态
  → SetState 通知 Modifier
  → 属性、地块或游戏事件触发重新测试
```

### 1. Requirement 的五个问题

写代码前先回答：

1. **Subject 是谁？** 玩家、单位、城市、地块，还是别的对象？
2. **判断数据在哪里？** 对象字段、接口、Definitions 表，还是 Property 容器？
3. **初次测试何时发生？** 创建期间、回合开始，还是第一次事件之后？
4. **什么事件会使结果变化？** 属性写入、建路、改地形、占领城市等。
5. **原生对象能否复用？** 优先选择布局和生命周期最接近的 donor requirement。

Requirement 的难点通常不是比较表达式，而是把这五件事接入游戏已有的对象、factory 和事件系统。

### 2. SQL 是 C++ 的入口

SQL 至少需要四组记录：

```sql
INSERT INTO Requirements (RequirementId, RequirementType)
VALUES ('REQ_EXAMPLE', 'REQUIREMENT_MY_CUSTOM_REQ');

INSERT INTO RequirementArguments (RequirementId, Name, Value)
VALUES ('REQ_EXAMPLE', 'MyArgument', 'SomeValue');

INSERT INTO RequirementSets (RequirementSetId, RequirementSetType)
VALUES ('REQSET_EXAMPLE', 'REQUIREMENTSET_TEST_ALL');

INSERT INTO RequirementSetRequirements (RequirementSetId, RequirementId)
VALUES ('REQSET_EXAMPLE', 'REQ_EXAMPLE');
```

然后把 requirement set 接到 Modifier：

```sql
INSERT INTO Modifiers
    (ModifierId, ModifierType, SubjectRequirementSetId)
VALUES
    ('MODIFIER_EXAMPLE', 'MODIFIER_PLAYER_ADJUST_PLOT_YIELD', 'REQSET_EXAMPLE');
```

注意：

- `RequirementType` 必须与 factory 的 `GetTypeName()` 完全一致；
- `RequirementId` 是实例名，`RequirementType` 是类/工厂类型名；
- 参数名称必须与 C++ 解析的 key 完全一致；
- 先用一个简单 Modifier 做烟测，不要一开始接复杂效果；
- 数据库日志没有报错，只能说明 SQL 接受了，不能证明 C++ requirement 已经正确执行。

### 3. 选择 donor requirement

通常不应该从零构造一个 C++ requirement 对象。应选择一个原生 donor：

| 自定义目标 | 合适 donor | 复用内容 |
|---|---|---|
| 玩家/单位属性 | `PlotPropertyMatches` 或布局相近的 Property requirement | `SimpleRequirement`、上下文、shared_ptr 生命周期 |
| 地块字段 | `PlotTerrainTypeMatches` | `PlotRequirement`、地块 subject、`TestPlot` 调用链 |
| 城市数值 | 原生 City/Player 数值 requirement | 城市接口和对应 signal |

选择 donor 时要核对：

- 对象大小和关键字段偏移；
- `OnInitialize` 参数签名；
- `Test` 或 `TestPlot` 的返回值编码；
- donor factory 的 vtable；
- donor 使用的 subject interface GUID；
- donor 的 signal/事件刷新方式。

只知道类名或一个 RVA 不够。必须知道它在对象生命周期中的位置。

### 4. 反编译时必须记录的资料

为每个版本的 GameCore DLL 建立地址表，至少记录：

```text
函数名 / 符号名
当前 Steam RVA（仅数据/vtable 或名称解析备用）
函数签名
所属对象或类
调用时机
关键字段偏移
返回值含义
```

重点包括：

- factory `GetTypeName`、`Create`、`ValidateDefinition` 的 vtable slot；
- donor `OnInitialize` 和 `Test`/`TestPlot`；
- `RequirementDefinition::GetArguments()`；
- ArgumentProcessor 的 `GetStringValue` 或 `GetDefinitionIndex`；
- subject interface 的 GUID 和取对象的 vtable slot；
- 修改数据的原生函数，例如 `SetRouteType`；
- 对应的 signal dispatcher。

RVA 只是地址定位信息。确认 RVA 正确后，还要确认：

1. 参数数量和寄存器约定是否正确；
2. 返回值是 `bool`、普通整数还是枚举；
3. 函数调用时对象是否已经构造完成；
4. 是否会在内部同步调用其他虚函数。

### 5. factory 的通用实现方式

最安全的模式是：

```text
找到 donor factory
  → 复制 donor factory vtable 到自己的可写存储
  → 只替换 GetTypeName / Create / ValidateDefinition
  → 把自己的 factory 追加到 registry vector
```

factory 的职责很小：

- `GetTypeName()` 返回新的 SQL 类型名；
- `ValidateDefinition()` 只接受新的 requirement type hash；
- `Create()` 调用 donor 的 native Create，获得完整对象和 shared_ptr。

不要在 `Create()` 中使用 `operator new` 自己猜对象大小，也不要自己拼接 `shared_ptr` 控制块。

当前项目中 route requirement 使用 `PlotTerrainTypeMatches` 的 Create，但不替换 requirement 实例的 vtable。这保留了原生：

- 对象分配；
- 构造函数；
- `enable_shared_from_this`；
- signal 连接；
- 引用计数；
- 析构过程。

### 6. 参数解析的通用原则

Requirement 参数来自 `RequirementParameters`，不要把它当成普通 C 结构体随意读取。

推荐流程：

```text
params
  → definition accessor
  → GetArguments()
  → 构造 ArgumentProcessor 临时对象
  → 使用游戏自己的 GetStringValue / GetDefinitionIndex
  → 转换成 requirement 自己保存的整数或 hash
```

必须注意：

- 临时 ArgumentProcessor 要使用正确的 vtable；
- 参数中的 shared pointer 必须增加并释放引用计数；
- 如果返回的是字符串，要在其所有权仍然有效时复制到本地 buffer；
- 不要把稀疏的 `GetDefinitionIndex` 误用于不匹配的 Definitions collection；
- 参数解析失败时应返回安全的 NotMet 或让创建失败，不要写入猜测值。

当前 route 实现先解析：

```text
RouteType = "ROUTE_ANCIENT_ROAD"
```

再转成 route index。当前版本还使用 RAII 释放临时 shared pointer，见源码中的 `SharedPointerPairGuard`。

### 7. Create 时序是最容易出错的地方

原生 factory 的 `Create()` 可能是：

```text
分配对象
  → Initialize
  → 第一次 Test
  → 返回 shared_ptr
```

因此“先调用 donor Create，返回后再设置字段”不一定正确。第一次 Test 可能已经发生。

如果自定义字段在 Create 期间还不可见，应使用临时上下文，例如：

```cpp
thread_local int pendingIndex = -1;

pendingIndex = parsedValue;
nativeCreate(...);
restore_previous_pendingIndex();
```

当前 route 实现使用 `PendingRouteIndexScope`，原因是 native Create 内部的第一次 `TestPlot` 发生在对象被加入 weak registry 之前。

如果存在嵌套 Create，不能简单地在结束时写死 `-1`，必须恢复进入前的值。

### 8. Test、TestPlot 和 RequirementState

自定义判断应尽量只做三件事：

1. 找到 subject；
2. 读取当前值；
3. 返回 Met 或 NotMet。

不同层可能使用不同编码。例如当前 route 代码中：

```text
TestPlot:             2 = Met, 3 = NotMet
PlotRequirement::Test: 0 = Met, 1 = NotMet
```

因此需要显式转换，不能把所有返回值直接交给 `SetState`。

推荐写法：

```text
原生/自定义 TestPlot
  → 读取对象字段
  → 返回该层规定的枚举值
  → 外层 Test 转换成 RequirementState
  → SetState
```

遇到空 subject、缺失参数、无效 index 时，默认返回 NotMet，不要访问空指针。

### 9. 对象生命周期：不要保存裸指针

Requirement 会被游戏删除、重建或随 GameCore reload 失效。只保存裸指针会造成 use-after-free。

当前项目的通用做法是利用 native `enable_shared_from_this` 布局：

```text
对象 + 0x08 → weak object
对象 + 0x10 → weak control block
```

注册时保存 weak control block，使用时：

```text
TryLockWeakReference
  → 成功增加 strong count
  → 使用对象
  → ReleaseStrongReference
```

事件刷新期间必须先锁定 strong reference，再访问对象；不能在遍历过程中直接相信旧的裸指针仍然有效。

### 10. 结果变化后必须主动刷新

Requirement 创建时测试一次并不代表以后会自动重新测试。

需要找到真正改变判断数据的事件：

| 判断数据 | 常见刷新点 |
|---|---|
| Player Property | Property dispatcher、Lua `Player:SetProperty` |
| 地块道路 | `SetRouteType`、窄范围 route signal |
| 城市人口/宜居度 | 对应城市修改函数或城市 signal |
| 地形/资源 | 地块修改函数、地图编辑事件 |

刷新流程通常是：

```text
调用原生修改函数
  → 原生数据已经写入
  → 找到受影响的 requirement
  → 重新 Test
  → 调用 SetState
```

刷新 hook 应尽量选择窄的、语义明确的修改函数。不要优先 hook 一个所有消息共用的底层 `Dispatch`，否则容易遇到错误 ABI、递归和高频开销。

### 11. 全局 hook 的安全边界

全局 hook 本身不可怕，关键是必须过滤：

```text
如果是自定义 requirement → 使用自定义逻辑
否则                 → 完整调用原函数
```

识别自定义对象可以使用：

- definition type hash；
- weak registry 中的对象地址；
- 明确的 factory/type name。

不要让自定义 hook 改变所有原生 requirement 的行为。

特别避免：

- 猜测实例 vtable 长度并复制整张 vtable；
- 覆盖原生对象的 RTTI/Complete Object Locator；
- 在错误的继承层函数上 hook；
- 用错误的成员函数签名接收参数。

### 12. Route index、Terrain index 等枚举值的通用处理

Definitions 类型参数通常不是 hash，而是 Definitions collection 中的下标。

通用步骤：

```text
SQL 名称
  → ArgumentProcessor 取得字符串
  → GetDefinitions() 取得 vector
  → 比较每个 definition 的类型名
  → index = iterator - begin
  → 写入对象字段
```

当前 route requirement 中：

```text
plot + 0x47 = 当前地块实际 route index
requirement + 0xA8 = 期望 route index
```

因此 `ROUTE_ANCIENT_ROAD` 对应的 `0` 本质上是它在游戏 RouteDefinitions 表中的位置。硬编码原版顺序在当前代码中是定义表扫描未找到时的 fallback，不能作为唯一实现。

## 当前 Loader 兼容性与重载验证

- `dll/HookAPI.h` 与当前 Civ6DllLoader 的 `common/HookAPI.h` 同步；升级时复制整份头文件并重新编译。
- Debug/Release 均定义 `CIV6_PLUGIN_GAMECORE`，由头文件生成 `Civ6PluginKind` 导出；`OnLoad` 首先检查 ABI。
- 从仓库根执行上面的构建命令。VS 2026 本机只有 v145 时可追加 `/p:PlatformToolset=v145`；工程默认保留 v143。
- 将产物放在已启用 Mod 的 `Binaries/Win64/Plugins/GameCore/`，配合本次更新的 Loader 测试 XP2。
- 同一游戏进程内反复执行“进对局 → 触发功能 → 保存 → 返回主菜单 → 读档 → 再次触发功能”。
- 检查 `Civ6DllLoader_Loader.log` 中的插件加载、卸载排队、引擎卸载后释放、再次加载，以及本插件的成功日志；编译通过不能代替游戏内验证。
- `OnUnload` 在 GameCore 仍映射时释放两类 Requirement registry 持有的 native weak references；刷新快照独立持有弱引用，清理后拒绝新增跟踪。
- factory/vtable 保留给引擎后续 teardown；插件 DLL 由 Loader 在 GameCore 卸载完成后释放。
- 验证玩家属性跨过 SQL 阈值时 Requirement 会刷新，以及建路/改道路后地块收益变化；读档后再次执行。
