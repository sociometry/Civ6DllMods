# GovernorAPI

> 分类：**APIExtension** · 返回 [工程总览](../../../README.md)

给模组作者补上「设置玩家可用总督点数」的 Lua 接口。

## 为什么需要

原版 Lua 只能**读**总督点（`GetGovernorPoints` / `GetGovernorPointsSpent`），
没有任何写入口。模组想给玩家补一个总督头衔，只能靠伪造事件之类的迂回手段。

## 接口

```lua
Player:GetGovernors():SetAvailableGovPoints(n)   --> boolean
```

| 参数 | 含义 |
| --- | --- |
| `n` | 目标**可用**点数。负数钳到 0。`double` 按整数截断（传 2.7 得 2） |

非法参数（类型不是 number、对象取不到）返回 `false`，**不抛 Lua 错误**。

```lua
Players[0]:GetGovernors():SetAvailableGovPoints(3)
print(Players[0]:GetGovernors():GetGovernorPoints())   -- 已花 + 3
```

## 语义：设的是「可用」，不是「总量」

引擎**只存总量**（`Governors+0x118`），"可用"是 UI 侧算出来的差值：

```
available = total - GetGovernorPointsSpent()
```

所以「把可用设成 n」等价于「把总量设成 spent + n」：

```
spent     = GetGovernorPointsSpent(govs)        // 纯读，遍历 Governor 列表累加头衔数
newTotal  = spent + n
delta     = newTotal - 当前 total
ChangeGovernorPoints(govs, delta)               // 交给引擎
```

### 为什么走引擎的 ChangeGovernorPoints，而不是直写字段

`Governors::ChangeGovernorPoints(int delta)` 内部（反汇编核实）已经做完所有收尾：

| 它做的事 | 直写字段的后果 |
| --- | --- |
| `IsMajor(pPlayer)` 才改动 | 城邦 / 蛮族也会被改 |
| 正增量时 clamp 总量 | 需要自己判断上限 |
| `HandleGovernorTitleMessaging()` | 标题提示不刷新 |
| `Event::Send(0xDA3EF23D, playerID, delta)` | 界面不刷新（要自己拼事件 hash） |

也就是说直写没有任何好处，只有副作用。`delta == 0` 时也照样调一次 ——
引擎自己就是无条件发事件的，界面靠它刷新。

## 内存布局（反汇编核实）

```
Governors + 0x070 = Player::Instance*   pPlayer
Governors + 0x118 = int                 governorPointsTotal（已花 + 可用）
```

## 目标

| 目标 | 方式 | AOB |
| --- | --- | --- |
| `Player::Governors::ChangeGovernorPoints` | `HookName` + AOB | 20 字节，唯一 |
| `Player::Governors::GetGovernorPointsSpent` | `HookName` + AOB | 16 字节，唯一 |
| `ScopedVirtualInstance<IPlayerGovernors, Governors>::GetInstance` | `HookName` | **不带**（与 Unit / Plot 取参函数序言逐字节相同） |

## 注册环境

用 `PushLuaMethods` 挂到原版 `PlayerGovernors` 元表上（引擎自己调
`IPlayerGovernors::PushMethods` 时由 Loader 追加），因此**只存在于 gameplay 环境**。

⚠️ `sigType` 必须是 `kHookApiPushWithManager`：该函数的签名是
`PushMethods(this, L, stackOffset)`，首参 `rcx` 是被丢弃的对象指针，`L` 在 `rdx`。
选错不会报错，但参数会整体错位。

## 日志

```
[GovernorAPI] active: SetAvailableGovPoints pushed=1
[GovernorAPI] SetAvailableGovPoints: spent=2 total 2 -> 5 (delta=3)
```
