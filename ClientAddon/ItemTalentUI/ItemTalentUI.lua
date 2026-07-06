-- ItemTalentUI: панель талантов предмета (mod-item-talents), клиент 3.3.5a.
--
-- Оболочка в стиле TransmogUI: кнопка на окне персонажа (рядом с кнопкой
-- трансмогрификации), слева колонка слотов экипировки, справа вертикальное
-- дерево талантов выбранного предмета. Открытие: кнопка на окне персонажа
-- или /itu. Ctrl+ПКМ убран (обработчики кликов 3.3.5 биндятся к шаблону
-- при загрузке XML - raw-замена глобала их не перехватывает).
--
-- Связь с сервером: команды ".itemtalent ..." через SendChatMessage(SAY),
-- ответы - SYSTEM-сообщения с префиксом "ITALENT:" (прячутся фильтром чата).

-- ---------------------------------------------------------------------------
-- Статические данные
-- ---------------------------------------------------------------------------

local ROWS_META = {
    { label = "ЗАТОЧКА",     r = 1.00, g = 1.00, b = 1.00 },
    { label = "ЗАКАЛКА",     r = 0.12, g = 1.00, b = 0.00 },
    { label = "ГРАВИРОВКА",  r = 0.00, g = 0.44, b = 0.87 },
    { label = "НАСЫЩЕНИЕ",   r = 0.64, g = 0.21, b = 0.93 },
    { label = "ПРОБУЖДЕНИЕ", r = 1.00, g = 0.50, b = 0.00 },
}

-- Слоты экипировки, участвующие в системе (клиентские inv-слоты)
local SLOT_LIST = {
    { inv = 1,  key = "HeadSlot",          name = "Голова" },
    { inv = 3,  key = "ShoulderSlot",      name = "Плечи" },
    { inv = 15, key = "BackSlot",          name = "Спина" },
    { inv = 5,  key = "ChestSlot",         name = "Грудь" },
    { inv = 9,  key = "WristSlot",         name = "Запястья" },
    { inv = 10, key = "HandsSlot",         name = "Кисти рук" },
    { inv = 6,  key = "WaistSlot",         name = "Пояс" },
    { inv = 7,  key = "LegsSlot",          name = "Ноги" },
    { inv = 8,  key = "FeetSlot",          name = "Ступни" },
    { inv = 16, key = "MainHandSlot",      name = "Правая рука" },
    { inv = 17, key = "SecondaryHandSlot", name = "Левая рука" },
    { inv = 18, key = "RangedSlot",        name = "Дальний бой" },
}

-- effect-код сервера -> иконка + шаблон описания (значение приходит в OPT)
local EFFECTS = {
    STAT_STA           = { icon = "Spell_Holy_WordFortitude",        fmt = "+%d к выносливости" },
    STAT_AGI           = { icon = "Ability_Hunter_AspectOfTheMonkey", fmt = "+%d к ловкости" },
    ATTACK_POWER       = { icon = "Ability_Warrior_BattleShout",     fmt = "+%d к силе атаки (ближней и дальней)" },
    SPELL_POWER        = { icon = "Spell_Holy_MagicalSentry",        fmt = "+%d к силе заклинаний" },
    RESIST_ALL         = { icon = "Spell_Nature_SpiritArmor",        fmt = "+%d к сопротивлению всем школам магии" },
    BLOCK_VALUE        = { icon = "Ability_Defend",                  fmt = "+%d к блокированию" },
    RATING_CRIT        = { icon = "Ability_CriticalStrike",          fmt = "+%d к рейтингу крит. удара" },
    RATING_HASTE       = { icon = "Spell_Nature_BloodLust",          fmt = "+%d к рейтингу скорости" },
    RATING_HIT         = { icon = "Ability_Marksmanship",            fmt = "+%d к рейтингу меткости" },
    RATING_DODGE       = { icon = "Ability_Rogue_Feint",             fmt = "+%d к рейтингу уклонения" },
    RATING_DEFENSE     = { icon = "Ability_Warrior_DefensiveStance", fmt = "+%d к рейтингу защиты" },
    RATING_PARRY       = { icon = "Ability_Parry",                   fmt = "+%d к рейтингу парирования" },
    RATING_BLOCK       = { icon = "INV_Shield_06",                   fmt = "+%d к рейтингу блока" },
    RATING_ARMOR_PEN   = { icon = "Ability_Warrior_Sunder",          fmt = "+%d к рейтингу пробивания брони" },
    SPELL_PEN          = { icon = "Spell_Arcane_ArcanePotency",      fmt = "+%d к проникающей способности заклинаний" },
    MP5                = { icon = "Spell_Magic_ManaGain",            fmt = "+%d к восполнению маны (за 5 сек)" },
    DURA_SAVE          = { icon = "Trade_BlackSmithing",             fmt = "Предмет теряет прочность на %d%% медленнее" },
    MOVE_SPEED_PCT     = { icon = "Ability_Rogue_Sprint",            fmt = "+%d%% к скорости передвижения" },
    HEAL_TAKEN_PCT     = { icon = "Spell_Holy_Renew",                fmt = "+%d%% к получаемому лечению" },
    PHYS_TAKEN_PCT     = { icon = "Spell_Holy_DevotionAura",         fmt = "-%d%% получаемого физического урона" },
    NEMESIS_DMG_PCT    = { icon = "INV_Misc_Bone_HumanSkull_01",     fmt = "+%d%% урона по немезидам" },
    FAMILIAR_ALL_STATS = { icon = "Ability_Hunter_BeastCall",        fmt = "Пока фамильяр призван: +%d ко всем характеристикам" },
    GOLD_XP_PCT        = { icon = "INV_Misc_Coin_02",                fmt = "+%d%% золота с существ, +1%% опыта" },
}
local FALLBACK_ICON = "INV_Misc_QuestionMark"

local ERR_TEXT = {
    DISABLED       = "Система талантов отключена на сервере.",
    BAD_ARGS       = "Внутренняя ошибка запроса (BAD_ARGS).",
    NO_ITEM        = "Предмет не найден. Возможно, он был перемещён.",
    NOT_EQUIPPED   = "Предмет должен быть надет.",
    NO_POOL        = "Этот предмет не участвует в системе талантов.",
    BROKEN         = "Предмет сломан - сначала почините его.",
    ROW_LOCKED     = "Ряд закрыт: не хватает качества предмета.",
    ROW_SOON       = "Этот ряд откроется в следующем обновлении.",
    BAD_CHOICE     = "Такого таланта нет.",
    ALREADY_CHOSEN = "В этом ряду уже сделан выбор.",
    NO_POINTS      = "Нет свободных очков таланта.",
    NO_MASTER      = "Нужен мастер оружия рядом (столицы).",
    NOT_CHOSEN     = "В этом ряду ничего не выбрано.",
}

-- v1-ограничение сервера: ряды выше не принимают выбор (ROW_SOON).
-- Продублировано с ItemTalents.MaxImplementedRow - поднимать вместе.
local MAX_IMPLEMENTED_ROW = 2

-- ---------------------------------------------------------------------------
-- Состояние
-- ---------------------------------------------------------------------------

local current = nil      -- разобранный info-блок выбранного предмета
local pending = nil      -- накапливаемый блок HDR..END
local selectedInv = nil  -- выбранный inv-слот
local resetMode = false
local lastInfoAt = 0
local invChangedAt = 0
local popupAction = nil

local slotButtons = {}   -- [inv] = button
local nodes = {}         -- [row][choice] = button
local wires = {}         -- пул текстур-сегментов пути
local rowLocked = {}     -- [row] = FontString пометки справа от ряда

local function Msg(text)
    DEFAULT_CHAT_FRAME:AddMessage("|cffc8a24b[Таланты]|r " .. text, 1.0, 0.85, 0.4)
end

local function SendCmd(cmd)
    SendChatMessage(cmd, "SAY")
end

local function EffectMeta(effect)
    return EFFECTS[effect] or { icon = FALLBACK_ICON, fmt = "%d" }
end

local function EffectDesc(effect, value)
    return string.format(EffectMeta(effect).fmt, value)
end

-- ---------------------------------------------------------------------------
-- Главное окно (оболочка в стиле TransmogUI)
-- ---------------------------------------------------------------------------

local f = CreateFrame("Frame", "ItemTalentUIFrame", UIParent)
f:SetWidth(620)
f:SetHeight(640)
f:SetPoint("CENTER")
f:SetFrameStrata("HIGH")
f:SetMovable(true)
f:EnableMouse(true)
f:SetClampedToScreen(true)
f:RegisterForDrag("LeftButton")
f:SetScript("OnDragStart", function(self) self:StartMoving() end)
f:SetScript("OnDragStop", function(self)
    self:StopMovingOrSizing()
    local point, _, relPoint, x, y = self:GetPoint()
    ItemTalentUIDB.pos = { point = point, relPoint = relPoint, x = x, y = y }
end)
f:SetBackdrop({
    bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
    edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
    tile = true, tileSize = 32, edgeSize = 32,
    insets = { left = 11, right = 12, top = 12, bottom = 11 },
})
f:Hide()
tinsert(UISpecialFrames, "ItemTalentUIFrame")

local close = CreateFrame("Button", nil, f, "UIPanelCloseButton")
close:SetPoint("TOPRIGHT", -5, -8)

local title = f:CreateFontString(nil, "OVERLAY", "GameFontNormal")
title:SetPoint("TOP", 0, -16)
title:SetText("|cffc8a24bПробуждение снаряжения|r")

-- Разделитель между колонкой слотов и деревом
local divider = f:CreateTexture(nil, "ARTWORK")
divider:SetTexture(0.45, 0.4, 0.25, 0.35)
divider:SetWidth(1)
divider:SetPoint("TOPLEFT", 92, -44)
divider:SetPoint("BOTTOMLEFT", 92, 24)

-- ---------------------------------------------------------------------------
-- Колонка слотов экипировки (слева)
-- ---------------------------------------------------------------------------

local SelectSlot -- forward

local function UpdateSlotButtons()
    for _, def in ipairs(SLOT_LIST) do
        local btn = slotButtons[def.inv]
        local tex = GetInventoryItemTexture("player", def.inv)
        -- Иконка ставится ТОЛЬКО через SetItemButtonTexture: SetNormalTexture
        -- у ItemButtonTemplate - это 64px регион подложки, иконка в нём
        -- рисуется гигантской (проверено 2026-07-06)
        SetItemButtonTexture(btn, tex or btn.emptyTex)
        local icon = _G[btn:GetName() .. "IconTexture"]
        if icon then
            if tex then
                icon:SetVertexColor(1, 1, 1)
            else
                icon:SetVertexColor(0.4, 0.4, 0.4)
            end
        end
        if selectedInv == def.inv then
            btn.sel:Show()
        else
            btn.sel:Hide()
        end
    end
end

local slotIndex = 0
local function CreateSlotButton(def)
    slotIndex = slotIndex + 1
    local btn = CreateFrame("Button", "ItemTalentUISlot" .. slotIndex, f, "ItemButtonTemplate")
    btn.def = def
    local _, emptyTex = GetInventorySlotInfo(def.key)
    btn.emptyTex = emptyTex

    btn.sel = btn:CreateTexture(nil, "OVERLAY")
    btn.sel:SetTexture("Interface\\Buttons\\CheckButtonHilight")
    btn.sel:SetBlendMode("ADD")
    btn.sel:SetAllPoints()
    btn.sel:Hide()

    btn:SetScript("OnClick", function(self) SelectSlot(self.def.inv) end)
    btn:SetScript("OnEnter", function(self)
        GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
        local link = GetInventoryItemLink("player", self.def.inv)
        if link then
            GameTooltip:SetHyperlink(link)
        else
            GameTooltip:SetText(self.def.name)
            GameTooltip:AddLine("Слот пуст", 0.65, 0.66, 0.72)
        end
        GameTooltip:Show()
    end)
    btn:SetScript("OnLeave", function() GameTooltip:Hide() end)

    slotButtons[def.inv] = btn
    return btn
end

for i, def in ipairs(SLOT_LIST) do
    local btn = CreateSlotButton(def)
    btn:SetPoint("TOPLEFT", 30, -52 - (i - 1) * 44)
end

-- ---------------------------------------------------------------------------
-- Правая область: шапка предмета + полоса опыта
-- ---------------------------------------------------------------------------

local headIcon = f:CreateTexture(nil, "ARTWORK")
headIcon:SetWidth(38)
headIcon:SetHeight(38)
headIcon:SetPoint("TOPLEFT", 110, -48)
headIcon:SetTexCoord(0.07, 0.93, 0.07, 0.93)

local headName = f:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
headName:SetPoint("TOPLEFT", headIcon, "TOPRIGHT", 10, -1)
headName:SetPoint("RIGHT", f, "RIGHT", -36, 0)
headName:SetJustifyH("LEFT")

local headSub = f:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
headSub:SetPoint("TOPLEFT", headName, "BOTTOMLEFT", 0, -4)
headSub:SetPoint("RIGHT", f, "RIGHT", -24, 0)
headSub:SetJustifyH("LEFT")
headSub:SetTextColor(0.65, 0.66, 0.72)

local xpLabel = f:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
xpLabel:SetPoint("TOPLEFT", 110, -98)
xpLabel:SetJustifyH("LEFT")

local xpRight = f:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
xpRight:SetPoint("TOPRIGHT", -24, -98)
xpRight:SetJustifyH("RIGHT")
xpRight:SetTextColor(0.65, 0.66, 0.72)

local xpBar = CreateFrame("StatusBar", nil, f)
xpBar:SetPoint("TOPLEFT", 110, -114)
xpBar:SetPoint("TOPRIGHT", -24, -114)
xpBar:SetHeight(9)
xpBar:SetStatusBarTexture("Interface\\TargetingFrame\\UI-StatusBar")
xpBar:SetStatusBarColor(1.0, 0.82, 0.0)
xpBar:SetMinMaxValues(0, 1)
local xpBg = xpBar:CreateTexture(nil, "BACKGROUND")
xpBg:SetAllPoints()
xpBg:SetTexture(0, 0, 0, 0.7)

-- ---------------------------------------------------------------------------
-- Дерево: линии пути (слой под узлами)
-- ---------------------------------------------------------------------------

local wireLayer = CreateFrame("Frame", nil, f)
wireLayer:SetAllPoints()

local function GetWire(i)
    if not wires[i] then
        wires[i] = wireLayer:CreateTexture(nil, "BORDER")
    end
    return wires[i]
end

local function HideWires(fromIndex)
    for i = fromIndex, #wires do wires[i]:Hide() end
end

-- Геометрия дерева (координаты от TOPLEFT окна)
local TREE_TOP = -164            -- y центра первого ряда
local ROW_STEP = 70
local NODE_X = { 250, 345, 440 } -- x центров узлов
local NODE_SIZE = 40
local LABEL_RIGHT_X = 206        -- правый край подписей рядов
local LOCKNOTE_X = 472           -- левый край пометок "качество"/"скоро"

local function NodeCenter(row, choice)
    return NODE_X[choice], TREE_TOP - (row - 1) * ROW_STEP
end

local function PlaceSegment(idx, x1, y1, x2, y2, gold)
    local t = GetWire(idx)
    if gold then
        t:SetTexture(1.0, 0.82, 0.0, 0.9)
    else
        t:SetTexture(0.4, 0.42, 0.48, 0.5)
    end
    local thickness = 2
    t:ClearAllPoints()
    if x1 == x2 then
        t:SetWidth(thickness)
        t:SetHeight(math.abs(y2 - y1))
        t:SetPoint("TOP", f, "TOPLEFT", x1, math.max(y1, y2))
    else
        t:SetWidth(math.abs(x2 - x1))
        t:SetHeight(thickness)
        t:SetPoint("TOPLEFT", f, "TOPLEFT", math.min(x1, x2), y1 + thickness / 2)
    end
    t:Show()
end

local function RedrawWires()
    local idx = 1
    if current then
        for row = 1, 4 do
            local a = current.rows[row].chosen
            local b = current.rows[row + 1] and current.rows[row + 1].chosen or 0
            if a > 0 and b > 0 then
                local x1, y1 = NodeCenter(row, a)
                local x2, y2 = NodeCenter(row + 1, b)
                local half = NODE_SIZE / 2
                local midY = y1 - half - (ROW_STEP - NODE_SIZE) / 2
                if x1 == x2 then
                    PlaceSegment(idx, x1, y1 - half, x2, y2 + half, true); idx = idx + 1
                else
                    PlaceSegment(idx, x1, y1 - half, x1, midY, true); idx = idx + 1
                    PlaceSegment(idx, x1, midY, x2, midY, true); idx = idx + 1
                    PlaceSegment(idx, x2, midY, x2, y2 + half, true); idx = idx + 1
                end
            end
        end
    end
    HideWires(idx)
end

-- ---------------------------------------------------------------------------
-- Узлы дерева
-- ---------------------------------------------------------------------------

local hint -- объявлен ниже (футер)

local function SetNodeState(btn, state)
    btn.state = state
    local icon, ring, glow, lock = btn.icon, btn.ring, btn.glow, btn.lock
    icon:SetDesaturated(false)
    icon:SetVertexColor(1, 1, 1)
    icon:SetAlpha(1)
    glow:Hide()
    glow.pulse:Stop()
    lock:Hide()

    if state == "chosen" then
        ring:SetVertexColor(1.0, 0.82, 0.0)
        glow:SetAlpha(0.7)
        glow:Show()
    elseif state == "avail" then
        ring:SetVertexColor(0.62, 0.90, 0.36)
        glow:SetAlpha(0.5)
        glow:Show()
        glow.pulse:Play()
    elseif state == "open" then -- ряд открыт, но нет свободных очков
        ring:SetVertexColor(0.42, 0.55, 0.38)
        icon:SetVertexColor(0.7, 0.7, 0.7)
    elseif state == "dim" then  -- в ряду уже есть другой выбор
        ring:SetVertexColor(0.32, 0.34, 0.40)
        icon:SetDesaturated(true)
        icon:SetAlpha(0.4)
    else                        -- locked
        ring:SetVertexColor(0.25, 0.27, 0.33)
        icon:SetDesaturated(true)
        icon:SetAlpha(0.55)
        icon:SetVertexColor(0.5, 0.5, 0.5)
        lock:Show()
    end
end

local function NodeOnEnter(self)
    if not current then return end
    local row, choice = self.row, self.choice
    local opt = current.rows[row].opts[choice]
    GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
    if not opt then
        GameTooltip:SetText("Пробуждение", 1, 0.5, 0)
        GameTooltip:AddLine("Откроется в следующих обновлениях.", 0.65, 0.66, 0.72, 1)
        GameTooltip:Show()
        return
    end

    GameTooltip:SetText(opt.name, 1.0, 0.82, 0.0)
    GameTooltip:AddLine(EffectDesc(opt.effect, opt.value), 1, 1, 1, 1)

    local state = self.state
    if state == "chosen" then
        GameTooltip:AddLine("Выбрано", 1.0, 0.82, 0.0)
        if current.nearMaster == 1 then
            GameTooltip:AddLine("ПКМ или режим сброса: сбросить ряд (бесплатно)", 0.65, 0.66, 0.72)
        end
    elseif state == "avail" then
        GameTooltip:AddLine("Доступно: клик для выбора (стоит 1 очко)", 0.62, 0.90, 0.36)
        if current.nearMaster ~= 1 then
            GameTooltip:AddLine("Выбор - только рядом с мастером оружия", 1.0, 0.35, 0.35)
        end
    elseif state == "open" then
        GameTooltip:AddLine(string.format("Ряд открыт - нет свободных очков (до очка: %d убийств)",
            math.max(0, (current.nextNeed or 0) - (current.kills or 0))), 0.79, 0.66, 0.29)
    elseif state == "dim" then
        local chosenOpt = current.rows[row].opts[current.rows[row].chosen]
        GameTooltip:AddLine("В этом ряду выбрано: " .. (chosenOpt and chosenOpt.name or "?"),
            0.65, 0.66, 0.72)
    else
        if row > current.rowsOpen then
            GameTooltip:AddLine("Закрыто: не хватает качества предмета", 0.8, 0.25, 0.25)
        elseif row > current.maxRow then
            GameTooltip:AddLine("Откроется в следующем обновлении", 0.8, 0.66, 0.29)
        else
            GameTooltip:AddLine("Закрыто", 0.8, 0.25, 0.25)
        end
    end
    GameTooltip:Show()
end

local function AskConfirm(text, action)
    popupAction = action
    -- текст уходит аргументом в "%s" - иначе '%' из описаний талантов
    -- сломает string.format внутри StaticPopup_Show
    StaticPopup_Show("ITEMTALENTUI_CONFIRM", text)
end

local function NodeOnClick(self, button)
    if not current then return end
    local row, choice = self.row, self.choice
    local state = self.state
    local rowData = current.rows[row]
    local opt = rowData.opts[choice]

    if state == "locked" then
        if row > current.rowsOpen then
            hint:SetText("Ряд закрыт: прокачайте качество предмета (Gear Ascension).")
        elseif row > current.maxRow then
            hint:SetText("Этот ряд откроется в следующем обновлении.")
        else
            hint:SetText("Ряд закрыт.")
        end
        return
    end

    if state == "open" then
        hint:SetText(string.format("Нет свободных очков - до следующего очка %d убийств.",
            math.max(0, (current.nextNeed or 0) - (current.kills or 0))))
        return
    end

    if state == "dim" then
        hint:SetText("В этом ряду уже сделан выбор - сброс бесплатно у мастера оружия.")
        return
    end

    if state == "chosen" then
        if button == "RightButton" or resetMode then
            resetMode = false
            if current.nearMaster ~= 1 then
                hint:SetText("Сброс - только рядом с мастером оружия.")
                return
            end
            local guid, r = current.guid, row
            AskConfirm(string.format("Сбросить ряд \"%s\"?\nОчко таланта вернётся (бесплатно).",
                ROWS_META[row].label), function()
                SendCmd(string.format(".itemtalent reset %d %d", guid, r))
            end)
        end
        return
    end

    -- state == "avail"
    if current.nearMaster ~= 1 then
        hint:SetText("Выбор таланта - только рядом с мастером оружия (столицы).")
        return
    end
    if opt then
        local guid, r, c = current.guid, row, choice
        AskConfirm(string.format("Выбрать |cffffd100%s|r?\n%s\n\nБудет потрачено 1 очко таланта."
            .. "\nСбросить можно бесплатно у мастера оружия.",
            opt.name, EffectDesc(opt.effect, opt.value)), function()
            SendCmd(string.format(".itemtalent choose %d %d %d", guid, r, c))
        end)
    end
end

local function CreateNode(row, choice)
    local btn = CreateFrame("Button", nil, f)
    btn:SetWidth(NODE_SIZE)
    btn:SetHeight(NODE_SIZE)
    local x, y = NodeCenter(row, choice)
    btn:SetPoint("CENTER", f, "TOPLEFT", x, y)
    btn.row = row
    btn.choice = choice

    -- Пропорции кнопки миникарты (LibDBIcon): иконка 17px, кольцо 53px,
    -- центр видимого круга смещён на (15.5, 14.5) от TOPLEFT текстуры -
    -- сам круг нарисован НЕ по центру холста MiniMap-TrackingBorder.
    local iconSize = 24
    local s = iconSize / 17

    local icon = btn:CreateTexture(nil, "ARTWORK")
    icon:SetWidth(iconSize)
    icon:SetHeight(iconSize)
    icon:SetPoint("CENTER")
    icon:SetTexCoord(0.08, 0.92, 0.08, 0.92)
    btn.icon = icon

    local ring = btn:CreateTexture(nil, "OVERLAY")
    ring:SetTexture("Interface\\Minimap\\MiniMap-TrackingBorder")
    ring:SetWidth(53 * s)
    ring:SetHeight(53 * s)
    ring:SetPoint("TOPLEFT", btn, "CENTER", -15.5 * s, 14.5 * s)
    btn.ring = ring

    local glow = btn:CreateTexture(nil, "OVERLAY", nil, 1)
    glow:SetTexture("Interface\\Buttons\\UI-ActionButton-Border")
    glow:SetBlendMode("ADD")
    glow:SetWidth(68)
    glow:SetHeight(68)
    glow:SetPoint("CENTER")
    glow:Hide()
    btn.glow = glow

    local pulse = glow:CreateAnimationGroup()
    local a = pulse:CreateAnimation("Alpha")
    a:SetChange(-0.4)
    a:SetDuration(0.8)
    pulse:SetLooping("BOUNCE")
    glow.pulse = pulse

    local lock = btn:CreateTexture(nil, "OVERLAY", nil, 2)
    lock:SetTexture("Interface\\LFGFrame\\UI-LFG-ICON-LOCK")
    lock:SetWidth(14)
    lock:SetHeight(14)
    lock:SetPoint("BOTTOMRIGHT", 2, -2)
    lock:SetTexCoord(0, 0.71875, 0, 0.875)
    lock:Hide()
    btn.lock = lock

    btn:RegisterForClicks("LeftButtonUp", "RightButtonUp")
    btn:SetScript("OnEnter", NodeOnEnter)
    btn:SetScript("OnLeave", function() GameTooltip:Hide() end)
    btn:SetScript("OnClick", NodeOnClick)
    return btn
end

for row = 1, 5 do
    local meta = ROWS_META[row]
    local _, y = NodeCenter(row, 1)

    local label = f:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    label:SetPoint("RIGHT", f, "TOPLEFT", LABEL_RIGHT_X, y)
    label:SetJustifyH("RIGHT")
    label:SetText(meta.label)
    label:SetTextColor(meta.r, meta.g, meta.b)

    local locked = f:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    locked:SetPoint("LEFT", f, "TOPLEFT", LOCKNOTE_X, y)
    locked:SetTextColor(0.5, 0.52, 0.58)
    locked:Hide()
    rowLocked[row] = locked

    nodes[row] = {}
    for choice = 1, 3 do
        nodes[row][choice] = CreateNode(row, choice)
    end
end

local function SetTreeShown(shown)
    for row = 1, 5 do
        if not shown then rowLocked[row]:Hide() end
        for choice = 1, 3 do
            if shown then
                nodes[row][choice]:Show()
            else
                nodes[row][choice]:Hide()
            end
        end
    end
    if not shown then HideWires(1) end
end

-- ---------------------------------------------------------------------------
-- Футер
-- ---------------------------------------------------------------------------

local summary = f:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
summary:SetPoint("BOTTOMLEFT", 110, 80)
summary:SetPoint("BOTTOMRIGHT", -24, 80)
summary:SetJustifyH("LEFT")
summary:SetTextColor(0.81, 0.90, 0.66)

local resetBtn = CreateFrame("Button", nil, f, "UIPanelButtonTemplate")
resetBtn:SetWidth(140)
resetBtn:SetHeight(22)
resetBtn:SetPoint("BOTTOMLEFT", 110, 50)
resetBtn:SetText("Сбросить ряд")
resetBtn:SetScript("OnClick", function()
    if not current then return end
    if current.nearMaster ~= 1 then
        hint:SetText("Сброс - только рядом с мастером оружия.")
        return
    end
    resetMode = true
    hint:SetText("Кликните по золотому узлу ряда, который нужно сбросить.")
end)

local closeBtn = CreateFrame("Button", nil, f, "UIPanelButtonTemplate")
closeBtn:SetWidth(140)
closeBtn:SetHeight(22)
closeBtn:SetPoint("BOTTOMRIGHT", -24, 50)
closeBtn:SetText("Закрыть")
closeBtn:SetScript("OnClick", function() f:Hide() end)

hint = f:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
hint:SetPoint("BOTTOMLEFT", 110, 28)
hint:SetPoint("BOTTOMRIGHT", -24, 28)
hint:SetJustifyH("LEFT")
hint:SetTextColor(0.55, 0.57, 0.63)

StaticPopupDialogs["ITEMTALENTUI_CONFIRM"] = {
    text = "%s",
    button1 = "Принять",
    button2 = "Отмена",
    OnAccept = function()
        if popupAction then popupAction() end
        popupAction = nil
    end,
    OnCancel = function() popupAction = nil end,
    timeout = 0,
    whileDead = 1,
    hideOnEscape = 1,
}

-- ---------------------------------------------------------------------------
-- Рендер
-- ---------------------------------------------------------------------------

local function RenderEmpty(text)
    current = nil
    headIcon:SetTexture("Interface\\Icons\\" .. FALLBACK_ICON)
    headName:SetText("Выберите предмет")
    headName:SetTextColor(0.8, 0.8, 0.8)
    headSub:SetText(text or "Кликните по слоту экипировки слева.")
    xpLabel:SetText("")
    xpRight:SetText("")
    xpBar:SetValue(0)
    summary:SetText("")
    SetTreeShown(false)
end

local function Render()
    if not current then return end

    local link = GetInventoryItemLink("player", selectedInv or 0)
    local itemName = link and link:match("%[(.-)%]") or ("Предмет " .. current.guid)

    headIcon:SetTexture(GetInventoryItemTexture("player", selectedInv or 0)
        or "Interface\\Icons\\" .. FALLBACK_ICON)
    local qc = ITEM_QUALITY_COLORS[current.quality] or ITEM_QUALITY_COLORS[1]
    headName:SetText(itemName)
    headName:SetTextColor(qc.r, qc.g, qc.b)
    -- разделитель '-': у клиента 3.3.5 нет глифа '·' (рисуется как '?')
    headSub:SetText(string.format("%s - ilvl %d - Ряды: %d из 5 - Убийств: %d",
        _G["ITEM_QUALITY" .. current.quality .. "_DESC"] or "?", current.ilvl,
        current.rowsOpen, current.kills))

    xpLabel:SetText(string.format("Очки таланта: |cffffd100%d|r", current.freePts))
    if current.nextNeed > 0 then
        xpRight:SetText(string.format("до следующего очка: %d / %d убийств",
            current.kills, current.nextNeed))
        xpBar:SetMinMaxValues(0, current.nextNeed)
        xpBar:SetValue(math.min(current.kills, current.nextNeed))
    else
        xpRight:SetText("предмет полностью прокачан")
        xpBar:SetMinMaxValues(0, 1)
        xpBar:SetValue(1)
    end

    SetTreeShown(true)
    for row = 1, 5 do
        local rowData = current.rows[row]
        local lockedLabel = rowLocked[row]
        lockedLabel:Hide()

        for choice = 1, 3 do
            local btn = nodes[row][choice]
            local opt = rowData.opts[choice]
            local meta = opt and EffectMeta(opt.effect) or { icon = FALLBACK_ICON }
            btn.icon:SetTexture("Interface\\Icons\\" .. meta.icon)

            local state
            if row > current.rowsOpen or (not opt and row >= 5) or row > current.maxRow then
                state = "locked"
            elseif rowData.chosen == choice then
                state = "chosen"
            elseif rowData.chosen > 0 then
                state = "dim"
            elseif current.freePts > 0 then
                state = "avail"
            else
                state = "open"
            end
            SetNodeState(btn, state)
        end

        if row > current.rowsOpen then
            lockedLabel:SetText("качество")
            lockedLabel:Show()
        elseif row > current.maxRow and rowData.chosen == 0 then
            lockedLabel:SetText("скоро")
            lockedLabel:Show()
        end
    end

    local parts = {}
    for row = 1, 5 do
        local rowData = current.rows[row]
        if rowData.chosen > 0 and rowData.opts[rowData.chosen] then
            local opt = rowData.opts[rowData.chosen]
            tinsert(parts, EffectDesc(opt.effect, opt.value))
        end
    end
    if #parts > 0 then
        summary:SetText("|cffffd100Итог:|r " .. table.concat(parts, " - "))
    else
        summary:SetText("|cffffd100Итог:|r таланты не выбраны")
    end

    resetBtn:SetAlpha(current.nearMaster == 1 and 1 or 0.5)
    RedrawWires()
end

-- ---------------------------------------------------------------------------
-- Выбор слота / обновление
-- ---------------------------------------------------------------------------

local function Refresh()
    if selectedInv then
        SendCmd(string.format(".itemtalent info inv %d", selectedInv))
    end
end

SelectSlot = function(inv)
    selectedInv = inv
    resetMode = false
    hint:SetText("")
    UpdateSlotButtons()

    if not GetInventoryItemLink("player", inv) then
        RenderEmpty("Слот пуст - наденьте предмет.")
        return
    end
    lastInfoAt = GetTime()
    SendCmd(string.format(".itemtalent info inv %d", inv))
end

local function ShowPanel()
    f:Show()
    UpdateSlotButtons()
    -- автоселект: прежний слот, иначе правая рука, иначе первый занятый
    local pick = selectedInv
    if not pick or not GetInventoryItemLink("player", pick) then
        pick = nil
        if GetInventoryItemLink("player", 16) then
            pick = 16
        else
            for _, def in ipairs(SLOT_LIST) do
                if GetInventoryItemLink("player", def.inv) then
                    pick = def.inv
                    break
                end
            end
        end
    end
    if pick then
        SelectSlot(pick)
    else
        RenderEmpty()
    end
end

-- Автообновление раз в 10 секунд, пока панель открыта (kills растут в бою)
f:SetScript("OnUpdate", function()
    if current and GetTime() - lastInfoAt > 10 then
        lastInfoAt = GetTime()
        Refresh()
    end
end)

-- ---------------------------------------------------------------------------
-- Протокол
-- ---------------------------------------------------------------------------

local function ParseLine(msg)
    if msg == "ITALENT:END" then
        if pending then
            pending.maxRow = MAX_IMPLEMENTED_ROW
            current = pending
            pending = nil
            lastInfoAt = GetTime()
            Render()
        end
        return true
    end

    if msg == "ITALENT:OK" then
        hint:SetText("Готово!")
        PlaySound("LEVELUPSOUND")
        return true
    end

    local err = msg:match("^ITALENT:ERR:(%w+)$")
    if err then
        local text = ERR_TEXT[err] or ("Ошибка: " .. err)
        if f:IsShown() then hint:SetText(text) else Msg(text) end
        if err == "NO_POOL" and f:IsShown() then
            RenderEmpty("Этот предмет не участвует в системе талантов.")
        end
        return true
    end

    local guid, ilvl, quality, pool, rowsOpen, nearMaster, kills, freePts, nextNeed =
        msg:match("^ITALENT:HDR:(%d+):(%d+):(%d+):(%a):(%d+):(%d+):(%d+):(%d+):(%d+)$")
    if guid then
        pending = {
            guid = tonumber(guid), ilvl = tonumber(ilvl), quality = tonumber(quality),
            pool = pool, rowsOpen = tonumber(rowsOpen), nearMaster = tonumber(nearMaster),
            kills = tonumber(kills), freePts = tonumber(freePts), nextNeed = tonumber(nextNeed),
            rows = {},
        }
        for row = 1, 5 do pending.rows[row] = { chosen = 0, opts = {} } end
        return true
    end

    if pending then
        local row, chosen = msg:match("^ITALENT:ROW:(%d+):(%d+)$")
        if row then
            pending.rows[tonumber(row)].chosen = tonumber(chosen)
            return true
        end

        local oRow, oChoice, effect, value, name =
            msg:match("^ITALENT:OPT:(%d+):(%d+):([%w_]+):(%-?%d+):(.+)$")
        if oRow then
            pending.rows[tonumber(oRow)].opts[tonumber(oChoice)] = {
                effect = effect, value = tonumber(value), name = name,
            }
            return true
        end
    end

    -- ITEM-строки списка и прочее протокольное - просто прячем
    return msg:find("^ITALENT:") ~= nil
end

ChatFrame_AddMessageEventFilter("CHAT_MSG_SYSTEM", function(self, event, msg)
    if msg and msg:find("^ITALENT:") then
        ParseLine(msg)
        return true -- спрятать из чата
    end
    return false
end)

-- ---------------------------------------------------------------------------
-- Кнопка на окне персонажа (рядом с кнопкой трансмогрификации)
-- ---------------------------------------------------------------------------

local charBtn = CreateFrame("Button", "ItemTalentUICharButton", CharacterFrame)
charBtn:SetWidth(28)
charBtn:SetHeight(28)
-- Слева окна персонажа (справа заняты кнопки TransmogUI и других аддонов)
charBtn:SetPoint("TOPLEFT", CharacterFrame, "TOPLEFT", 70, -35)
charBtn:SetFrameLevel(CharacterFrame:GetFrameLevel() + 5)
charBtn:SetNormalTexture("Interface\\Icons\\Ability_Marksmanship")
charBtn:GetNormalTexture():SetTexCoord(0.07, 0.93, 0.07, 0.93)
charBtn:SetHighlightTexture("Interface\\Buttons\\ButtonHilight-Square", "ADD")

local charBtnBorder = charBtn:CreateTexture(nil, "OVERLAY")
charBtnBorder:SetTexture("Interface\\Buttons\\UI-Quickslot2")
charBtnBorder:SetPoint("CENTER")
charBtnBorder:SetWidth(50)
charBtnBorder:SetHeight(50)

charBtn:SetScript("OnClick", function()
    if f:IsShown() then
        f:Hide()
    else
        ShowPanel()
    end
end)
charBtn:SetScript("OnEnter", function(self)
    GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
    GameTooltip:SetText("Пробуждение снаряжения")
    GameTooltip:AddLine("Таланты предметов: качество открывает ряды,", 0.8, 0.8, 0.8)
    GameTooltip:AddLine("убийства копят очки.", 0.8, 0.8, 0.8)
    GameTooltip:Show()
end)
charBtn:SetScript("OnLeave", function() GameTooltip:Hide() end)

-- ---------------------------------------------------------------------------
-- События / слэш
-- ---------------------------------------------------------------------------

local ev = CreateFrame("Frame")
ev:RegisterEvent("PLAYER_LOGIN")
ev:RegisterEvent("UNIT_INVENTORY_CHANGED")
ev:SetScript("OnEvent", function(self, event, arg1)
    if event == "PLAYER_LOGIN" then
        ItemTalentUIDB = ItemTalentUIDB or {}
        if ItemTalentUIDB.pos then
            f:ClearAllPoints()
            f:SetPoint(ItemTalentUIDB.pos.point, UIParent, ItemTalentUIDB.pos.relPoint,
                ItemTalentUIDB.pos.x, ItemTalentUIDB.pos.y)
        end
        RenderEmpty()
        Msg("загружен. Кнопка на окне персонажа или /itu.")
    elseif event == "UNIT_INVENTORY_CHANGED" and arg1 == "player" and f:IsShown() then
        UpdateSlotButtons()
        local now = GetTime()
        if now - invChangedAt > 1 then
            invChangedAt = now
            if selectedInv and GetInventoryItemLink("player", selectedInv) then
                Refresh()
            end
        end
    end
end)

SLASH_ITEMTALENTUI1 = "/itu"
SLASH_ITEMTALENTUI2 = "/itemtalent"
SlashCmdList["ITEMTALENTUI"] = function(arg)
    local inv = tonumber(arg)
    if inv and inv >= 1 and inv <= 19 then
        if not f:IsShown() then f:Show(); UpdateSlotButtons() end
        SelectSlot(inv)
    elseif f:IsShown() then
        f:Hide()
    else
        ShowPanel()
    end
end
