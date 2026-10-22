# GreatWorkYieldStack

> 中文 | [English](README.en.md)

> 分类：**BugFix** · 返回 [工程总览](../../../README.md)

让同种巨作来自多个来源的产出加成能够叠加。

## 问题

`City::Culture::GetGreatWorkYieldChange(this, objType, yieldType)` 的实现是**命中首条即返回**：

```c
for (int* p = this->+0x68; p < this->+0x70; p += 0x10) {
    if (p[0] == objType && p[1] == yieldType && p[2] > 0)
        return p[2];        // ← 命中第一条就返回，后面的同类型全部忽略
}
return 0;
```

遍历区间 `[this+0x68, this+0x70)`，元素步长 **0x10**，每项前三个 int 是
`{objectType, yieldType, amount}`。

**后果**：当同一 `(objType, yieldType)` 有多条来源时（比如两件「著作」各自 +3 文化），
只有第一条生效，其余全被忽略。

唯一调用方是 `City::Buildings::GetBuildingYieldFromGreatWorks`（城市巨作产出结算）。

## 做法

hook `GetGreatWorkYieldChange`，把「命中首条即返回」改成**遍历求和**：

```c
int sum = 0;
for (每条 {objectType, yieldType, amount})
    if (匹配 && amount > 0) sum += amount;
return sum;
```

`amount > 0` 的过滤口径与参考实现一致，不会把负值也算进去。

## 与 CE 参考实现的差异（有意为之）

CE 的 `Hook_BB` 用 `int*` 每 4 字节滑动比较，靠「滑动窗口总会扫过对齐命中」实现求和。

本移植改为按**反汇编实证的真实步长 16 字节**遍历：

| 项目 | CE 做法 | 本模块 |
| --- | --- | --- |
| 步进 | 4 字节滑动 | **16 字节**（按实测步长） |
| 未对齐误配风险 | 有（可能匹配到跨字段的三元组） | 无 |
| 求和口径 | `amount > 0` 过滤后累加 | 同 |

语义等价，但免除了对未对齐三元组的误配可能。

## 安全网

游戏更新可能导致布局漂移，因此加了两层保护：

1. `begin` / `end` 指针无效或跨度过大（> 4096 项）时 → **回退原函数**；
2. 没有 trampoline 且布局校验失败时 → **中性返回 0**（不改变原版行为，只是仍不叠加）。

## 目标

| 目标 | RVA | 序言 |
| --- | --- | --- |
| `City::Culture::GetGreatWorkYieldChange` | — | `48 8B 41 68 4C 8B 49 70`（AOB 唯一命中） |

## 来源

从 CivilizationVI_CommunityExtension 1.3.0-beta 的 `Main.cpp`（`GET_BB_OFFSET=0x11C630`
+ `Hook_BB`）移植到 Civ6DllLoader 插件框架。

## 日志

```
[GreatWorkYieldStack] hooked GetGreatWorkYieldChange; great work yields now stack
```
