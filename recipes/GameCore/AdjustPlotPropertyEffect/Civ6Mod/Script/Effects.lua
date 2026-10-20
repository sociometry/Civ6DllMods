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
    -- This event provides coordinates and owner, but no removed improvement type.
    -- Log all removals so farms already present in a loaded save are covered too.
    PrintPlotProperty(x, y)
end

Events.ImprovementAddedToMap.Add(OnImprovementAddedToMap)
Events.ImprovementRemovedFromMap.Add(OnImprovementRemovedFromMap)
