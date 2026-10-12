-- FrontEndUI_YourFeature.lua
-- 与同目录 .xml 同名。Loader 调 Initialize(baseName) 后，ForgeUI 按 base name 加载本文件。
-- 控件布局在 .xml 里；本文件只负责逻辑。

local RootContainer
local TitleLabel

function OnInit()
    RootContainer = ContextPtr:GetControl("RootContainer")
    TitleLabel    = ContextPtr:LookUpControl("TitleLabel")
    print("FrontEndUI_YourFeature: context initialized")
    ContextPtr:SetHide(false)   -- mod context 初始是隐藏的，要显式显示
end

function OnShow()
    print("FrontEndUI_YourFeature: shown")
end

function OnInputHandler(input)
    if input:GetType() == Types.InputTypes.MOUSEUP and input:IsButtonClicked() then
        local control = ContextPtr:GetControl("OpenButton")
        if control ~= nil and control:IsSelected() then
            print("FrontEndUI_YourFeature: button pressed")
            ContextPtr:SetHide(true)
            return true
        end
    end
    return false
end

-- Context 关闭时清理。UI 侧事件（Events.*）必须在这里 .Remove()。
function OnShutdown()
    print("FrontEndUI_YourFeature: shutdown")
end

ContextPtr:SetInitHandler(OnInit)
ContextPtr:SetShowHandler(OnShow)
ContextPtr:SetInputHandler(OnInputHandler, true)
ContextPtr:SetShutdown(OnShutdown)
