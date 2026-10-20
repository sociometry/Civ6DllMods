# GreatWorkPreserve

> 分类：**BugFix** · 返回 [工程总览](../../../README.md)

修复「改变额外巨作槽数量时，该建筑已放置的巨作全部丢失」。

## 缺陷

`Buildings::ChangeExtraGreatWorkSlots` 在建筑**已建成**时成对调用两个函数：

```c
RemoveGreatWorkSlots(this, buildType);   // 删掉该建筑的巨作记录，槽数组一并释放
AddGreatWorkSlots(this, buildType);      // 只按定义重建【空槽】
```

`AddGreatWorkSlots` 从不读取旧内容，重建出来的每个 `greatWorkIndex` 都是 `-1`。
两次调用之间**没有任何数据传递** —— 所以该建筑上已放置的巨作全部丢失，且不归还玩家。

**触发场景**：`AdjustExtraGreatWorkSlots` 效果改变某建筑的巨作槽数量
（政策卡 / 奇观 / 伟人能力）。

## 修法：单个钩子 + 私有暂存

只钩 `ChangeExtraGreatWorkSlots`（**public** 成员，RVA `0x109C50`）：

```
Hook ChangeExtraGreatWorkSlots
  ├─ changeNum == 0 → 原样放行（引擎不会做任何重建）
  ├─ 入口：CapturePendingSlots(...)        把有效槽内容存进私有暂存
  ├─ 调原函数：内部 Remove → Add，重建出全空槽
  └─ 出口：RestorePendingSlots(...)        按槽位把暂存内容填回空槽
```

暂存是插件私有的 `std::vector`，不持有任何游戏堆指针。抓取发生在重建**之前**，
所以不依赖堆复用之类的巧合；出口用完立刻清空，不存在跨调用的残留。

### 为什么只钩一个

`ChangeExtraGreatWorkSlots` **只有一个出口**（函数体没有提前 `return`），
一次「入口存 + 出口填」即可完整覆盖。

早期版本钩的是它内部调用的那两个 private 函数（`Add`/`RemoveGreatWorkSlots`），
而那两者还有别的调用者，会被误伤：

| 调用者 | 是否需要保存/恢复 | 双钩子的后果 |
| --- | --- | --- |
| `ChangeExtraGreatWorkSlots` | ✅ 需要 | 正常 |
| `RemoveBuilding` / `RemoveAllBuildings` | ❌ 拆建筑，之后不会再 `Add` | 暂存一直残留，等下一个同 `buildType` 的 `Add` 误消费 |
| `SetBuildingLocation` | ❌ 建新建筑，本来就没有旧巨作 | 「恢复」多余 |

改成单钩子后这些问题都不存在，而且钩的是 public 符号，比同族 private 函数更稳。

## 三条红线（踩过坑，勿改）

1. **绝不改变 `greatWorkBuildings` 数组的结构** —— 不去重、不合并、不搬移元素、不改 `count`。
   早期版本用 `memcpy` 把尾项搬到中间再 `--count`，结果 `MOVE_GREAT_WORK` 期间按数组下标
   定位的写入全部错位，表现为「移动巨作后其它巨作位置乱掉」。
2. **只写 `greatWorkIndex`**，不改 `slotType`、数组指针、`count`。
3. **回填只填空槽**（`greatWorkIndex == -1`），不覆盖已有内容 —— 即使与 `MOVE_GREAT_WORK`
   交错，也不会把玩家刚移动的巨作冲掉。

## 回填的两轮策略

| 轮次 | 做法 |
| --- | --- |
| 第一轮 · 原位回归 | 原绝对下标仍在、`slotType` 匹配、该槽为空 → 放回原位置（最精确，同类别并存也不串位） |
| 第二轮 · 兼容空槽顺延 | 第一轮没放回去的，按 `slotType` 在剩余空槽里依次填入；每项只用一次、每槽只填一次 |

只有**第二轮也填不下**才真正丢数据 —— 那是容量真的不够，会逐条打
`LOST gwIndex=... slotType=...` 说明。

`slotType` 是**槽类别**（ART / WRITING / RELIC…），不是巨作类型 —— 一个著作槽能放任意著作，
宫殿槽也能放著作。因此同类别多槽时必须靠「下标」或「同类别内序号」区分，不能只看类型。

### 为什么不能只按「同类别内序号」配对

早期版本要求「暂存项在旧记录里是该类别第 N 个」与「新空槽在该类别里是第 N 个」相等。
槽位形状一变序就错位：

```
缩容：旧 [-1, 42]  →  新 [-1]
      42 是第 1 号（0-based），新空槽是第 0 号 → 序号对不上 → 直接丢弃
```

而剩余容量其实**足够**放下 42。这就是「有空间却静默丢数据」——比容量不足更糟，
因为玩家看不出原因。

现在的做法是：先原位回归，放不回去的再找同类别空槽，实在没有才报 LOST。

## 结构体布局（反编译核实）

```
Buildings (+0x170)
  +0x0C8  GreatWorkInfo*  greatWorkBuildings   // 元素 0x20 字节
  +0x0D8  int             greatWorkBuildingsCount

GreatWorkInfo (0x20)
  +0x00 int              BuildingType
  +0x08 GreatWorkSlot*   GreatWorks
  +0x10 int              GreatWorkSlotCount
  +0x18 int              GreatWorkNum

GreatWorkSlot (0x08)
  +0x00 GreatWorkSlotTypes  slotType
  +0x04 GreatWorkListIndex  greatWorkIndex   // -1 = 空
```

## 目标

| 目标 | 符号 | 说明 |
| --- | --- | --- |
| `Buildings::ChangeExtraGreatWorkSlots` | `HookName`（demangled）+ AOB | public，RVA `0x109C50`，序言 20 字节唯一命中 |

签名：`void ChangeExtraGreatWorkSlots(Buildings*, BuildingTypes, GreatWorkSlotTypes, int changeNum)`。

## 验证

先在某建筑的巨作槽里放几个巨作，再触发一次改槽数（带 `AdjustExtraGreatWorkSlots`
的政策卡 / 奇观），巨作应保留，日志出现：

```
[GreatWorkPreserve] bt=1 in-place=2 filled=1 restored=3/3
```

- `in-place`：原位放回的数量
- `filled`：靠第二轮顺延补进其他空槽的数量
- `restored/total`：最终保住 / 待恢复的总数

容量确实不足时会额外逐条打印：

```
[GreatWorkPreserve] bt=1 LOST gwIndex=42 slotType=7 (no compatible empty slot)
```

缩容场景的期望结果（旧 `[-1,42]` → 新 `[-1]`）：`restored=1/1`，42 落到下标 0。