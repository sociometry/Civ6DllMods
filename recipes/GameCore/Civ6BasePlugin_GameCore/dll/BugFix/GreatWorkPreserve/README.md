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

## 回填的两级策略

| 情况 | 做法 |
| --- | --- |
| 重建后槽的**数量与 `slotType` 序列完全一致** | 按绝对槽位下标还原，最精确 |
| 序列变了（本次改动改变了槽布局） | 退化为按 `slotType` 分组、组内顺序填充；宁可少填也不错填 |

`slotType` 是**槽类别**（ART / WRITING / RELIC…），不是巨作类型 —— 一个著作槽能放任意著作，
宫殿槽也能放著作。因此同类别多槽时必须靠「下标」或「同类别内序号」区分，不能只看类型。

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
[GreatWorkPreserve] buildingType=1 slotType=7 change=+2 restored=3
```

`restored` 就是成功保住的巨作数量。
