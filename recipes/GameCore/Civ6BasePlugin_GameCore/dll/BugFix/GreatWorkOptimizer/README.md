# GreatWorkOptimizer

> 中文 | [English](README.en.md)

> 分类：**BugFix** · 返回 [工程总览](../../../README.md)

缓解大后期 AI 计算巨作分配时的卡顿。

## 问题

AI 每回合要为每座城市的每栋建筑挑选「往哪个槽放哪件巨作」。原版实现是：

1. 遍历候选巨作 × 候选槽位，做一次全排列评估；
2. 每次评估都重新计算一遍槽位组合的产出价值。

复杂度约 **O(G³)**（G = 巨作数量）。到了大后期，一个文化型 AI 可能手握几十件巨作，
这套计算会吃掉一整帧的时间，表现为**过回合明显卡顿**。

## 做法

三个改点，逐条对应原版的热点：

| 改点 | 手段 |
| --- | --- |
| 复用计算结果 | 给 `CultureAi_GreatWorks::Execute` 加**结果缓存**，同一回合内重复调用直接命中 |
| 剪枝 | 槽位组合先用 FNV-1a 哈希归类，同构组合只算一次 |
| 钳制规模 | `bounded_knapsack` 里限制候选集大小，避免组合爆炸 |

对于**人类玩家**，这套重算没有意义（玩家自己决定放哪），因此检测到是本地人类时
**直接跳过**整个优化路径，走原版的全量重算 —— 保证人类侧行为与原版完全一致。

## 关键实现

### 人类判定不装钩

用 `Player::Instance::IsHuman` 直调用判断，**不 hook**。解析失败时人类跳过分支
自动失效（退回原版全量重算），不会误伤 AI 路径。

### 日志节流

重算路径本身就是性能热点，如果每次都打日志反而更慢。因此所有日志降为
**每 N 次 / 仅首次**打印。

## 目标

| 目标 | 方式 | AOB |
| --- | --- | --- |
| `CultureAi_GreatWorks::Execute` | `HookName` + AOB | 24 字节，唯一 |
| `bounded_knapsack` | `HookName` + AOB | 23 字节，唯一 |
| `Definitions::GetBuildingDefinitions` | `HookName` + AOB | 20 字节（含 `??`），唯一 |
| `Player::Instance::IsHuman` | `HookName` 直调用 | **不带**（7 处同形） |

## 槽位布局（反编译复核）

```
Execute:  this   +0x28  = Player::Instance*
          player +0x6D0 = Player::Cities
          cities +0xD0  = 城市链表头（node[0]=City*，node[2]=next）
          city   +0xD78 = GreatWorkInfo 数组，+0xD88 = 容量，元素 0x20 字节

GreatWorkInfo +0x00 = BuildingType
              +0x08 = int[2] / 槽：[0]=GreatWorkListIndex，[1]=theme（-1 = 空槽）

Definition::Building             +0x1C8 / +0x1D0 = Building_GreatWork 区间
Definition::Building_GreatWork   +0x24 = SlotNum
```

## 来源

从 CivilizationVI_CommunityExtension 1.3.0-beta 的 `GreatWorkOptimizer.cpp` 移植到
Civ6DllLoader 插件框架。逻辑本体（FNV-1a 哈希、Execute 缓存跳过、knapsack 钳制）
照抄参考实现，槽位布局已用当前 IDB 反汇编复核。

## 日志

```
[GreatWorkOptimizer] installed 2/2 hooks; buildingDefs=ok humanSkip=on
[GreatWorkOptimizer] player=1 gwCount=0 exec=1 cacheSkip=0 humanSkip=1
```
