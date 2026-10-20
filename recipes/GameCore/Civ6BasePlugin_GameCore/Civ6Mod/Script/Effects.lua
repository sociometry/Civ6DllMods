-- 地块动态属性观察器（调试用）：农场被建造/移除时打印 EXAMPLE_PROPERTY 的当前值。
-- 只读不写 —— 属性由 C++ 侧的 EFFECT_ADJUST_PLOT_PROPERTY 修改。

local function PrintPlotProperty(x, y)
    local plot = Map.GetPlot(x, y)
    if plot then
        print("EXAMPLE_PROPERTY", plot:GetProperty("EXAMPLE_PROPERTY"))
    end
end

local function OnImprovementAddedToMap(x, y, improvementType)
    local improvement = GameInfo.Improvements[improvementType]
    if improvement and improvement.ImprovementType == "IMPROVEMENT_FARM" then
        PrintPlotProperty(x, y)
    end
end

local function OnImprovementRemovedFromMap(x, y)
    -- 该事件只给坐标与所有者，不给被移除的改良类型；全部打印，
    -- 这样读档时已存在的农场也能被覆盖到。
    PrintPlotProperty(x, y)
end

Events.ImprovementAddedToMap.Add(OnImprovementAddedToMap)
Events.ImprovementRemovedFromMap.Add(OnImprovementRemovedFromMap)