-- The game builds a body part's treatment menu only from items you carry and hides it when nothing applies, so with
-- no bandage a wounded arm shows no menu at all. This lists what the injury needs, greyed out with the items it takes,
-- so the menu always opens. A left click (tap) on a body part opens it too, and right click works while paused.
require "XpSystem/ISUI/ISHealthPanel"

local function name(fullType)
    local item = getScriptManager():FindItem(fullType)
    return item and item:getDisplayName() or fullType
end

-- groups: { {"Base.Needle", "Base.Thread"}, {"Base.SutureNeedle"} } -> "Needle + Thread / Suture Needle"
local function names(groups)
    local out = {}
    for _, g in ipairs(groups) do
        local parts = {}
        for _, t in ipairs(g) do table.insert(parts, name(t)) end
        table.insert(out, table.concat(parts, " + "))
    end
    return table.concat(out, " / ")
end

local function needs(context, labelKey, groups)
    local label = getText(labelKey)
    if context:getOptionFromName(label) then return end
    local option = context:addOption(label .. " - " .. getText("IGUI_SwT_Needs", names(groups)), nil, nil)
    option.notAvailable = true
end

local function addMissing(context, bp)
    local injured = (bp:HasInjury() or bp:stitched() or bp:getSplintFactor() > 0) and not bp:bandaged()
    if not injured then return end
    needs(context, "ContextMenu_Bandage", { {"Base.Bandage"}, {"Base.RippedSheets"} })
    needs(context, "ContextMenu_Disinfect", { {"Base.Disinfectant"}, {"Base.AlcoholWipes"} })
    if bp:isDeepWounded() and not bp:haveGlass() then
        needs(context, "ContextMenu_Stitch", { {"Base.Needle", "Base.Thread"}, {"Base.SutureNeedle"} })
    end
    if bp:haveBullet() then
        needs(context, "ContextMenu_Remove_Bullet", { {"Base.Tweezers"}, {"Base.SutureNeedleHolder"} })
    end
    if bp:isNeedBurnWash() then
        needs(context, "ContextMenu_Clean_Burn", { {"Base.Bandage"} })
    end
    local t = bp:getType()
    if t ~= BodyPartType.Head and t ~= BodyPartType.Torso_Upper and t ~= BodyPartType.Torso_Lower and bp:HasInjury()
            and not bp:stitched() and bp:getSplintFactor() <= 0 and bp:getFractureTime() > 0 then
        needs(context, "ContextMenu_Splint", { {"Base.Splint"}, {"Base.Plank", "Base.RippedSheets"} })
    end
end

local doMenu = ISHealthPanel.doBodyPartContextMenu
function ISHealthPanel:doBodyPartContextMenu(bodyPart, x, y)
    doMenu(self, bodyPart, x, y)
    if self.blockingMessage then return end   -- patient too far away: the game's own rule
    local playerNum = self.otherPlayer and self.otherPlayer:getPlayerNum() or self.character:getPlayerNum()
    local context = getPlayerContextMenu(playerNum)
    addMissing(context, bodyPart)
    if context:isEmpty() or context:getIsVisible() then return end
    -- the game hid it as empty before we added to it: show it, and hand it the controller like the game does
    context:setVisible(true)
    local joypad = JoypadState.players[playerNum + 1]
    if joypad then
        context.mouseOver = 1
        context.origin = self
        joypad.focus = context
        updateJoypadFocus(joypad)
    end
end

local function openAt(list, x, y)
    local row = list:rowAt(x, y)
    if row < 1 or row > #list.items then return end
    list.selected = row
    list.parent:doBodyPartContextMenu(list.items[row].item.bodyPart, list:getX() + x, list:getY() + y)
end

local mouseUp = ISHealthBodyPartListBox.onMouseUp
function ISHealthBodyPartListBox:onMouseUp(x, y)
    local dragging = ISMouseDrag.dragging and #ISMouseDrag.dragging > 0
    mouseUp(self, x, y)
    if not dragging then openAt(self, x, y) end
end

-- the game ignores right click while paused (outside debug mode); queued treatments simply start on unpause
function ISHealthBodyPartListBox:onRightMouseUp(x, y)
    openAt(self, x, y)
end
