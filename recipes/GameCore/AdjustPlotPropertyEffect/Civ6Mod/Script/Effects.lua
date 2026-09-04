

Events.ImprovementAddedToMap = function(improvementType, x, y)
    local plot = Map.GetPlot(x, y)
    local improvement = GameInfo.Improvements[improvementType]
    if improvement.ImprovementType == "IMPROVEMENT_FARM" then
        print('EXAMPLE_PROPERTY', plot:GetProperty("EXAMPLE_PROPERTY"))
    end
end

Events.ImprovementRemovedFromMap = function(improvementType, x, y)
    local plot = Map.GetPlot(x, y)
    local improvement = GameInfo.Improvements[improvementType]
    if improvement.ImprovementType == "IMPROVEMENT_FARM" then
        print('EXAMPLE_PROPERTY', plot:GetProperty("EXAMPLE_PROPERTY"))
    end
end

