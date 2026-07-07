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
    STAT_STA           = { icon = "Spell_Nature_Reincarnation",      fmt = "+%d к выносливости" },
    STAT_STR           = { icon = "Spell_Nature_Strength",           fmt = "+%d к силе" },
    STAT_AGI           = { icon = "Ability_Hunter_AspectoftheMonkey", fmt = "+%d к ловкости" },
    STAT_INT           = { icon = "Spell_Holy_ArcaneIntellect",      fmt = "+%d к интеллекту" },
    STAT_SPI           = { icon = "Spell_Shadow_Burningspirit",      fmt = "+%d к духу" },
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
    JORDAN_COIN        = { icon = "INV_Misc_Coin_17",                fmt = "Убийства с шансом чеканят владельцу %d меди" },
    MANA_FLAT          = { icon = "Spell_Holy_MagicalSentry",        fmt = "+%d к максимуму маны" },
}
local FALLBACK_ICON = "INV_Misc_QuestionMark"

-- Проки ряда 5 (effect='PROC'): иконки и тексты по имени перка
-- (источник: CUSTOM_SPELLS.md, иконки утверждены 2026-07-06)
local PROCS = {
    ["Мановая пелена"]     = { icon = "Spell_Shadow_DetectLesserInvisibility", fmt = "Поглощает %d урона при ударе по вам" },
    ["Всплеск чар"]        = { icon = "Spell_Nature_WispSplode",       fmt = "+%d к силе заклинаний на 10 сек с каста" },
    ["Последний оплот"]    = { icon = "Spell_Holy_PowerWordShield",    fmt = "Щит %d при здоровье ниже 35%%" },
    ["Рывок тени"]         = { icon = "Ability_Rogue_Sprint",          fmt = "+30%% скорости бега при крите по вам" },
    ["Змеиная реакция"]    = { icon = "Ability_Hunter_SerpentSwiftness", fmt = "+%d ловкости при уклонении" },
    ["Контрудар"]          = { icon = "Spell_Nature_NatureTouchDecay", fmt = "%d урона природой при уклонении" },
    ["Разряд"]             = { icon = "Spell_Nature_Lightning",        fmt = "%d урона природой атакующему" },
    ["Заземление"]         = { icon = "Spell_Nature_GroundingTotem",   fmt = "+%d к сопротивлениям при маг. уроне" },
    ["Гальваника"]         = { icon = "Spell_Nature_LightningShield",  fmt = "+%d к рейтингу скорости за убийство" },
    ["Шипы стали"]         = { icon = "Spell_Nature_Thorns",           fmt = "%d физ. урона атакующему" },
    ["Второе дыхание"]     = { icon = "Ability_Hunter_Harass",         fmt = "Исцеляет %d при здоровье ниже 30%%" },
    ["Несгибаемость"]      = { icon = "Ability_Warrior_ShieldWall",    fmt = "+%d брони после тяжелого удара" },
    ["Отражение"]          = { icon = "Ability_Warrior_ShieldReflection", fmt = "%d физ. урона атакующему при блоке" },
    ["Стена героя"]        = { icon = "Ability_Warrior_ShieldMastery", fmt = "+%d к блокированию при блоке" },
    ["Живая сталь"]        = { icon = "Spell_Holy_HolyBolt",           fmt = "Исцеляет %d при блоке" },
    ["Ответный выпад"]     = { icon = "INV_Sword_27",                  fmt = "Шанс дополнительного удара" },
    ["Танец клинка"]       = { icon = "Ability_Whirlwind",             fmt = "После парирования след. удар +%d урона" },
    ["Дуэлянт"]            = { icon = "Ability_Warrior_Riposte",       fmt = "+%d к криту и парированию на 8 сек" },
    ["Кровавая жатва"]     = { icon = "Ability_Warrior_BloodFrenzy",   fmt = "+%d к силе атаки с крита" },
    ["Зазубренная кромка"] = { icon = "Ability_Rogue_Rupture",         fmt = "Кровотечение %d за 6 сек с крита" },
    ["Пир победителя"]     = { icon = "Ability_Warrior_Devastate",     fmt = "Исцеляет %d за убийство" },
    ["Оглушающий удар"]    = { icon = "Spell_Frost_Stun",              fmt = "Шанс оглушить цель на 2 сек" },
    ["Дробящий удар"]      = { icon = "Ability_Smash",                 fmt = "-%d брони цели на 8 сек" },
    ["Землетрясение"]      = { icon = "Spell_Nature_Earthquake",       fmt = "%d урона врагам в 5 м с крита" },
    ["Размах"]             = { icon = "Ability_Warrior_Cleave",        fmt = "Удар задевает до 2 доп. целей (%d)" },
    ["Подсечка"]           = { icon = "Ability_ShockWave",             fmt = "Шанс замедлить цель на 50%%" },
    ["Насаженный"]         = { icon = "INV_Spear_06",                  fmt = "Кровотечение %d за 6 сек с крита" },
    ["Теневой укол"]       = { icon = "Spell_Shadow_ShadowBolt",       fmt = "%d урона тьмой с удара" },
    ["Отравленная кромка"] = { icon = "Ability_Poisons",               fmt = "Яд %d за 6 сек с крита" },
    ["Скользящая тень"]    = { icon = "Ability_Vanish",                fmt = "+%d к рейтингу скорости за убийство" },
    ["Град ударов"]        = { icon = "Ability_GhoulFrenzy",           fmt = "Шанс 2 дополнительных ударов" },
    ["Глухая защита"]      = { icon = "Spell_Magic_LesserInvisibilty", fmt = "+%d к уклонению при ударе по вам" },
    ["Прямой в челюсть"]   = { icon = "Ability_Kick",                  fmt = "Шанс прервать заклинание цели" },
    ["Меткий выстрел"]     = { icon = "INV_Spear_07",                  fmt = "Доп. выстрел на %d урона" },
    ["Пригвождение"]       = { icon = "Ability_Rogue_Trip",            fmt = "Шанс замедлить цель на 50%%" },
    ["Хладнокровие"]       = { icon = "Spell_Frost_WizardMark",        fmt = "+%d к криту за убийство" },
    ["Картечь"]            = { icon = "Ability_UpgradeMoonGlaive",     fmt = "%d урона цели и 2 врагам рядом" },
    ["Оглушительный залп"] = { icon = "Ability_GolemStormBolt",        fmt = "-20%% скорости атаки цели с крита" },
    ["Пробивающий выстрел"]= { icon = "Ability_PierceDamage",          fmt = "-%d брони цели с крита" },
    ["Дуга силы"]          = { icon = "Spell_Arcane_StarFire",         fmt = "%d урона тайной магией со спелл-крита" },
    ["Эхо маны"]           = { icon = "Spell_Frost_ManaRecharge",      fmt = "Возвращает %d маны с каста" },
    ["Фамильяр-фантом"]    = { icon = "Spell_Nature_SpiritWolf",       fmt = "Шанс призвать духа-фантома на 15 сек" },
    ["Гнев Джордана"]      = { icon = "Spell_Nature_ChainLightning",   fmt = "Молния на %d урона природой с каста" },
}

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

-- Ряды 1-4 реализованы; ряд 5 доступен только именным предметам - у
-- остальных сервер просто не шлёт OPT-строки ряда 5, и узлы показываются
-- закрытыми через ветку "not opt" (константа больше не режет ряды).
local MAX_IMPLEMENTED_ROW = 5

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

local invCache = {}      -- [invSlot] = {guid, kills, level, spent} для тултипов (.itemtalent list)
local infoCache = {}     -- [itemGuid] = разобранный info-блок (+ .at) для мгновенного ре-рендера панели
local listBuild = nil    -- накапливаемый ответ list
local listReqAt = 0      -- троттлинг запросов list
local listAt = nil       -- время отложенного запроса list
local syncAt = nil       -- время отложенной верификации кэша (.itemtalent sync)
local didInitialSync = false -- разовое восстановление кэша за сессию
local syncPending = false -- ждём ответ на .itemtalent sync (для инвалидации деревьев)
local lastHash = nil     -- хеш перк-состояния из последней строки ITALENT:HASH
local charKey = nil      -- "Realm-Name": ключ локального кэша в ItemTalentUIDB.chars

local slotButtons = {}   -- [inv] = button
local nodes = {}         -- [row][choice] = button
local rowLabels = {}     -- [row] = FontString подписи ряда
local rowNotes = {}      -- [row] = FontString "уровень N" справа от ряда
local wires = {}         -- пул текстур-сегментов пути

local function Msg(text)
    DEFAULT_CHAT_FRAME:AddMessage("|cffc8a24b[Таланты]|r " .. text, 1.0, 0.85, 0.4)
end

local function SendCmd(cmd)
    SendChatMessage(cmd, "SAY")
end

-- Запрос состояний всей экипировки (для тултипов), не чаще раза в 5 сек
local function RequestList()
    if GetTime() - listReqAt < 5 then return end
    listReqAt = GetTime()
    SendCmd(".itemtalent list")
end

-- Локальный кэш тултипов между сессиями (SavedVariables). Валидность
-- подтверждает сервер по хешу (.itemtalent sync) при входе в мир:
-- совпало - кэш свежий, сервер досылает только kills; разошлось
-- (перки/экипировка изменились или кэш подправили руками) - полный list.
-- ТРОТТЛИНГ: сама запись - глубокое копирование всех деревьев infoCache, а
-- зовётся на каждый info-ответ (клик/авторефреш) -> клиентский подлаг панели.
-- Реально копируем не чаще раза в 15 сек; force=true (логаут) сбрасывает сразу.
local lastSaveAt = 0
local function SaveCache(force)
    if not charKey or not lastHash then return end
    if not force and GetTime() - lastSaveAt < 15 then return end
    lastSaveAt = GetTime()
    -- guid ОБЯЗАТЕЛЕН: им панельный кэш (infoCache) ключуется - без него после
    -- логина по SYNC:OK клики не попадают в кэш и снова бьют в сервер.
    local inv, guids = {}, {}
    for slot, st in pairs(invCache) do
        inv[slot] = { guid = st.guid, kills = st.kills, level = st.level, spent = st.spent }
        if st.guid then guids[st.guid] = true end
    end
    -- Полные деревья надетых предметов тоже персистим (ключ = GUID): при
    -- совпадении хеша на логине панель рисуется из них БЕЗ единого запроса.
    local trees = {}
    for guid, block in pairs(infoCache) do
        if guids[guid] then trees[guid] = block end
    end
    ItemTalentUIDB.chars = ItemTalentUIDB.chars or {}
    ItemTalentUIDB.chars[charKey] = { hash = lastHash, inv = inv, trees = trees }
end

local function EffectMeta(effect, name)
    if effect == "PROC" and name and PROCS[name] then
        return PROCS[name]
    end
    return EFFECTS[effect] or { icon = FALLBACK_ICON, fmt = "%d" }
end

local function EffectDesc(effect, value, name)
    local fmt = EffectMeta(effect, name).fmt
    if fmt:find("%%d") then
        return string.format(fmt, value)
    end
    return fmt
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

-- Версия аддона в правом нижнем углу панели (для быстрой сверки что загружено)
local verText = f:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
verText:SetPoint("BOTTOMRIGHT", f, "BOTTOMRIGHT", -14, 12)
verText:SetText("v" .. (GetAddOnMetadata and GetAddOnMetadata("ItemTalentUI", "Version") or "?"))
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
local NODE_SIZE = 55
local LABEL_RIGHT_X = 206        -- правый край подписей рядов

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

-- Цвет окантовки узла = КАЧЕСТВО ролла перка (обычный/отличный/совершенный)
local QUALITY_RING = {
    [0] = { r = 0.90, g = 0.90, b = 0.90 },
    [1] = { r = 0.12, g = 1.00, b = 0.00 },
    [2] = { r = 0.64, g = 0.21, b = 0.93 },
}

local function SetNodeState(btn, state, quality)
    btn.state = state
    local icon, ring, glow, lock = btn.icon, btn.ring, btn.glow, btn.lock
    icon:SetDesaturated(false)
    icon:SetVertexColor(1, 1, 1)
    icon:SetAlpha(1)
    glow:Hide()
    glow.pulse:Stop()
    lock:Hide()

    -- Окантовка показывает качество, состояние - яркость кольца + свечение
    local qc = QUALITY_RING[quality or 0] or QUALITY_RING[0]
    local dim = 1.0

    if state == "chosen" then
        glow:SetVertexColor(qc.r, qc.g, qc.b)  -- свечение = цвет качества узла
        glow:SetAlpha(0.65)
        glow:Show()
    elseif state == "avail" then
        glow:SetVertexColor(qc.r, qc.g, qc.b)  -- свечение = цвет качества узла
        glow:SetAlpha(0.5)
        glow:Show()
        glow.pulse:Play()
    elseif state == "open" then -- ряд ждёт свой уровень пробуждения
        dim = 0.7
        icon:SetVertexColor(0.7, 0.7, 0.7)
        lock:Show() -- будет доступен гриндом: замок, а не пустота
    elseif state == "dim" then  -- в ряду уже есть другой выбор
        dim = 0.4
        icon:SetDesaturated(true)
        icon:SetAlpha(0.4)
    else                        -- locked
        dim = 0.35
        icon:SetDesaturated(true)
        icon:SetAlpha(0.55)
        icon:SetVertexColor(0.5, 0.5, 0.5)
        lock:Show()
    end

    ring:SetVertexColor(qc.r * dim, qc.g * dim, qc.b * dim)
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
    GameTooltip:AddLine(EffectDesc(opt.effect, opt.value, opt.name), 1, 1, 1, 1)
    if opt.quality == 1 then
        GameTooltip:AddLine("Качество перка: отличное (+25%)", 0.12, 1.0, 0.0)
    elseif opt.quality == 2 then
        GameTooltip:AddLine("Качество перка: совершенное (+50%)", 0.64, 0.21, 0.93)
    end

    local state = self.state
    if state == "chosen" then
        GameTooltip:AddLine("Выбрано", 1.0, 0.82, 0.0)
        if current.nearMaster == 1 then
            GameTooltip:AddLine("ПКМ или режим сброса: сбросить ряд (бесплатно)", 0.65, 0.66, 0.72)
        end
    elseif state == "avail" then
        GameTooltip:AddLine("Доступно: клик для выбора", 0.62, 0.90, 0.36)
        if current.nearMaster ~= 1 then
            GameTooltip:AddLine("Выбор - только рядом с мастером оружия", 1.0, 0.35, 0.35)
        end
    elseif state == "open" then
        if row == (current.level or 0) + 1 then
            GameTooltip:AddLine(string.format(
                "Нужен уровень пробуждения %d (до него: %d убийств)",
                row, math.max(0, (current.nextNeed or 0) - (current.kills or 0))),
                0.79, 0.66, 0.29)
        else
            GameTooltip:AddLine(string.format("Нужен уровень пробуждения %d", row),
                0.79, 0.66, 0.29)
        end
    elseif state == "dim" then
        local chosenOpt = current.rows[row].opts[current.rows[row].chosen]
        GameTooltip:AddLine("В этом ряду выбрано: " .. (chosenOpt and chosenOpt.name or "?"),
            0.65, 0.66, 0.72)
    else
        if row == 5 and current.baseEpic == 0 then
            GameTooltip:AddLine("Недоступен: предмет не эпический по происхождению", 0.8, 0.25, 0.25)
        elseif row > current.rowsOpen then
            GameTooltip:AddLine(string.format(
                "Откроется на уровне пробуждения %d - нужно качество предмета выше", row),
                0.8, 0.25, 0.25)
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
        if row == 5 and current.baseEpic == 0 then
            hint:SetText("Пробуждение доступно только предметам, эпическим по происхождению.")
        elseif row > current.rowsOpen then
            hint:SetText("Ряд закрыт: прокачайте качество предмета (Gear Ascension).")
        elseif row > current.maxRow then
            hint:SetText("Этот ряд откроется в следующем обновлении.")
        else
            hint:SetText("Ряд закрыт.")
        end
        return
    end

    if state == "open" then
        hint:SetText(string.format(
            "Ряд %d откроется на уровне пробуждения %d - убивайте с этим предметом.",
            row, row))
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
        AskConfirm(string.format("Выбрать |cffffd100%s|r?\n%s\n\n"
            .. "Сбросить можно бесплатно у мастера оружия.",
            opt.name, EffectDesc(opt.effect, opt.value, opt.name)), function()
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

    -- MiniMap-TrackingBorder: видимый круг НЕ по центру холста, его центр
    -- смещён на (15.5, 14.5) от TOPLEFT текстуры 53px. РАЗМЕР кольца считаем
    -- от NODE_SIZE (а не от иконки - иначе кольцо растёт вместе с иконкой и та
    -- всегда остаётся одной долей отверстия = "мелкой"). scale = ring/53 держит
    -- круг концентричным с центром кнопки; иконка независимо заполняет отверстие.
    local ringSize = NODE_SIZE * 53 / 42   -- внешний круг ≈ размер узла
    local scale = ringSize / 51
    local iconSize = NODE_SIZE * 0.47      -- заполняет отверстие кольца 0.74

    local icon = btn:CreateTexture(nil, "ARTWORK")
    icon:SetWidth(iconSize)
    icon:SetHeight(iconSize)
    icon:SetPoint("CENTER")
    icon:SetTexCoord(0.08, 0.92, 0.08, 0.92)
    btn.icon = icon

    local ring = btn:CreateTexture(nil, "OVERLAY")
    ring:SetTexture("Interface\\Minimap\\MiniMap-TrackingBorder")
    ring:SetWidth(ringSize)
    ring:SetHeight(ringSize)
    ring:SetPoint("TOPLEFT", btn, "CENTER", -15.5 * scale, 14.5 * scale)
    btn.ring = ring

    -- Круглое радиальное свечение ПОД узлом (UI-ActionButton-Border
    -- квадратный и торчит углами из-под круглого кольца)
    local glow = btn:CreateTexture(nil, "BACKGROUND")
    glow:SetTexture("Interface\\Cooldown\\starburst")
    glow:SetBlendMode("ADD")
    glow:SetWidth(58)
    glow:SetHeight(58)
    glow:SetPoint("CENTER")
    glow:Hide()
    btn.glow = glow

    local pulse = glow:CreateAnimationGroup()
    local a = pulse:CreateAnimation("Alpha")
    a:SetChange(-0.4)
    a:SetDuration(0.8)
    pulse:SetLooping("BOUNCE")
    glow.pulse = pulse

    -- Замок в нижнем правом углу, на отдельном фрейме ПОВЕРХ кольца и
    -- свечения (sub-level у CreateTexture в 3.3.5 ненадёжен)
    local lockFrame = CreateFrame("Frame", nil, btn)
    lockFrame:SetAllPoints()
    lockFrame:SetFrameLevel(btn:GetFrameLevel() + 3)
    local lock = lockFrame:CreateTexture(nil, "OVERLAY")
    lock:SetTexture("Interface\\LFGFrame\\UI-LFG-ICON-LOCK")
    lock:SetWidth(14)
    lock:SetHeight(14)
    lock:SetPoint("BOTTOMRIGHT", -8, 10)
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
    rowLabels[row] = label

    local note = f:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    note:SetPoint("LEFT", f, "TOPLEFT", 470, y)
    note:SetTextColor(0.55, 0.57, 0.63)
    note:Hide()
    rowNotes[row] = note

    nodes[row] = {}
    for choice = 1, 3 do
        nodes[row][choice] = CreateNode(row, choice)
    end
end

local function SetTreeShown(shown)
    for row = 1, 5 do
        if shown then rowLabels[row]:Show() else rowLabels[row]:Hide() end
        if not shown then rowNotes[row]:Hide() end
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
    headSub:SetText(string.format("%s - ilvl %d - Убийств: %d",
        _G["ITEM_QUALITY" .. current.quality .. "_DESC"] or "?", current.ilvl,
        current.kills))

    xpLabel:SetText(string.format("Уровень пробуждения: |cffffd100%d|r из 5", current.level))
    if current.nextNeed > 0 then
        xpRight:SetText(string.format("до уровня %d: %d / %d убийств",
            current.level + 1, current.kills, current.nextNeed))
        xpBar:SetMinMaxValues(0, current.nextNeed)
        xpBar:SetValue(math.min(current.kills, current.nextNeed))
    else
        xpRight:SetText("предмет полностью пробуждён")
        xpBar:SetMinMaxValues(0, 1)
        xpBar:SetValue(1)
    end

    SetTreeShown(true)

    -- Финальная схема (2026-07-07): ряды выше качества предмета СКРЫТЫ
    -- (без GA-апгрейда недостижимы - показывать нечего), а ряды, ждущие
    -- своего уровня пробуждения, видны С ЗАМКОМ и подписью "уровень N" -
    -- их можно добить гриндом. rowsOpen уже учитывает базовых эпиков.
    for row = 1, 5 do
        local note = rowNotes[row]
        if row > current.rowsOpen then
            rowLabels[row]:Hide()
            note:Hide()
            for choice = 1, 3 do
                nodes[row][choice]:Hide()
            end
        elseif current.rows[row].chosen == 0 and (current.level or 0) < row then
            note:SetText(string.format("уровень %d", row))
            note:Show()
        else
            note:Hide()
        end
    end

    for row = 1, 5 do
        local rowData = current.rows[row]

        for choice = 1, 3 do
            local btn = nodes[row][choice]
            local opt = rowData.opts[choice]
            local meta = opt and EffectMeta(opt.effect, opt.name) or { icon = FALLBACK_ICON }
            btn.icon:SetTexture("Interface\\Icons\\" .. meta.icon)

            local state
            if row > current.rowsOpen or (not opt and row >= 5) or row > current.maxRow then
                state = "locked"
            elseif rowData.chosen == choice then
                state = "chosen"
            elseif rowData.chosen > 0 then
                state = "dim"
            elseif current.level >= row then
                state = "avail"
            else
                state = "open" -- ряд ждёт свой уровень пробуждения
            end
            SetNodeState(btn, state, opt and opt.quality or 0)
        end
    end

    local parts = {}
    for row = 1, 5 do
        local rowData = current.rows[row]
        if rowData.chosen > 0 and rowData.opts[rowData.chosen] then
            local opt = rowData.opts[rowData.chosen]
            tinsert(parts, EffectDesc(opt.effect, opt.value, opt.name))
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

-- itemId (entry) предмета в слоте из ссылки: |Hitem:12345:...|h. Нужен, чтобы
-- отличить сменившийся предмет - guid клиенту в 3.3.5 недоступен, а entry в
-- ссылке есть. Разные предметы = разный entry -> кэш слота устарел.
local function SlotItemId(inv)
    local link = GetInventoryItemLink("player", inv)
    if not link then return nil end
    return tonumber(link:match("|Hitem:(%d+):"))
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

    -- Дерево талантов из КЭША по GUID рисуем МГНОВЕННО (оно статично, меняется
    -- только на выбор/сброс) - без задержки и мигания. Но кэш слота (invCache)
    -- держит ПРЕЖНИЙ предмет, пока не придёт свежий list, поэтому применяем
    -- кэш ТОЛЬКО если entry предмета в слоте совпадает с живым - иначе на миг
    -- показался бы прогресс старого предмета (баг смены наплечников). Затем
    -- ВСЕГДА дозапрашиваем свежий блок: kills/мастер волатильны.
    local liveId = SlotItemId(inv)
    local st = invCache[inv]
    local guid = st and st.guid
    local cached = guid and infoCache[guid]
    if cached and cached.itemId ~= liveId then
        cached = nil -- кэш слота от прежнего предмета - не рисуем, ждём сервер
    end
    if ItemTalentUIDB and ItemTalentUIDB.debug then
        Msg(string.format("слот %d: invCache=%s guid=%s дерево=%s",
            inv, st and "да" or "НЕТ", tostring(guid), cached and "ЕСТЬ->кэш" or "нет->сервер"))
    end
    if cached then
        current = cached
        Render() -- мгновенно из кэша (дерево)
    else
        RenderEmpty("Загрузка...") -- чистим панель, чтобы не висел старый предмет
    end
    lastInfoAt = GetTime()
    SendCmd(string.format(".itemtalent info inv %d", inv)) -- свежие kills/мастер
end

local function ShowPanel()
    f:Show()
    UpdateSlotButtons()
    -- Данные по убийствам/уровню всех надетых предметов обновляем ОДИН раз
    -- при открытии панели (решение 2026-07-07) - без фонового поллинга.
    RequestList()
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

-- Данные (в т.ч. убийства/уровень) запрашиваются ОДИН РАЗ при открытии
-- панели/выборе слота и после действий выбора/сброса - без поллинга
-- (решение 2026-07-07). OnUpdate-автообновление убрано.

-- ---------------------------------------------------------------------------
-- Протокол
-- ---------------------------------------------------------------------------

local function ParseLine(msg)
    if msg == "ITALENT:END" then
        if pending then
            pending.maxRow = MAX_IMPLEMENTED_ROW
            pending.at = GetTime()
            -- entry предмета в выбранном слоте: по нему SelectSlot отличает
            -- сменившийся предмет и не рисует чужой кэш
            pending.itemId = SlotItemId(selectedInv or 0)
            infoCache[pending.guid] = pending -- кэш по GUID для мгновенного ре-рендера
            -- Применяем ответ, ТОЛЬКО если он для текущего выбранного слота
            -- (при быстром переключении медленный ответ по другому слоту не
            -- должен затирать картинку); при холодном invCache - применяем.
            local st = invCache[selectedInv or 0]
            if not (st and st.guid) or st.guid == pending.guid then
                current = pending
                lastInfoAt = GetTime()
                Render()
            end
            pending = nil
            SaveCache() -- персистим выученное дерево сразу (no-op пока нет lastHash)
        elseif listBuild then
            invCache = listBuild
            listBuild = nil
            -- Полный list в ответ на sync = хеш РАЗОШЁЛСЯ: персистнутые деревья
            -- могли устареть, сбрасываем (обычные list-обновления не трогают
            -- infoCache - там ключ по GUID сам отсекает сменившиеся предметы).
            if syncPending then
                infoCache = {}
                syncPending = false
            end
            SaveCache()
        end
        return true
    end

    -- Хеш перк-состояния (приходит между ITEM-строками и END ответа list)
    local h = msg:match("^ITALENT:HASH:(%d+)$")
    if h then
        lastHash = tonumber(h)
        return true
    end

    -- Кэш подтверждён: обновляем только kills (в хеш они не входят)
    local syncKills = msg:match("^ITALENT:SYNC:OK:?(.*)$")
    if syncKills then
        syncPending = false -- хеш совпал: персистнутые деревья валидны
        for slot, kills in syncKills:gmatch("(%d+)=(%d+)") do
            local st = invCache[tonumber(slot)]
            if st then st.kills = tonumber(kills) end
        end
        SaveCache()
        return true
    end

    if msg == "ITALENT:OK" then
        hint:SetText("Готово!")
        PlaySound("LEVELUPSOUND")
        listAt = GetTime() + 1 -- обновить кэш тултипов
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

    -- HDR: 10-е поле baseEpic добавлено фазой 2; старый 9-полевой формат
    -- тоже принимаем (переходный период), baseEpic тогда = 1
    local guid, ilvl, quality, pool, rowsOpen, nearMaster, kills, freePts, nextNeed, baseEpic =
        msg:match("^ITALENT:HDR:(%d+):(%d+):(%d+):(%a):(%d+):(%d+):(%d+):(%d+):(%d+):(%d+)$")
    if not guid then
        guid, ilvl, quality, pool, rowsOpen, nearMaster, kills, freePts, nextNeed =
            msg:match("^ITALENT:HDR:(%d+):(%d+):(%d+):(%a):(%d+):(%d+):(%d+):(%d+):(%d+)$")
    end
    if guid then
        pending = {
            guid = tonumber(guid), ilvl = tonumber(ilvl), quality = tonumber(quality),
            pool = pool, rowsOpen = tonumber(rowsOpen), nearMaster = tonumber(nearMaster),
            kills = tonumber(kills),
            -- поле протокола прежнее, но с 2026-07-06 это УРОВЕНЬ пробуждения
            -- (0..5): ряд N открывается только с уровня N
            level = tonumber(freePts), nextNeed = tonumber(nextNeed),
            baseEpic = baseEpic and tonumber(baseEpic) or 1,
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

        -- OPT:<row>:<slot>:<effect>:<value>:<quality>:<name>
        local oRow, oSlot, effect, value, quality, name =
            msg:match("^ITALENT:OPT:(%d+):(%d+):([%w_]+):(%-?%d+):(%d+):(.+)$")
        if oRow then
            pending.rows[tonumber(oRow)].opts[tonumber(oSlot)] = {
                effect = effect, value = tonumber(value),
                quality = tonumber(quality), name = name,
            }
            return true
        end
    end

    -- ITEM-строки ответа list: состояния надетых предметов для тултипов.
    -- guid захватываем (2-е поле) - им ключуется кэш info-блоков (infoCache).
    local iSlot, iGuid, iKills, iLevel, iSpent =
        msg:match("^ITALENT:ITEM:(%d+):(%d+):%a:%d+:(%d+):(%d+):(%d+):")
    if iSlot then
        listBuild = listBuild or {}
        listBuild[tonumber(iSlot)] = {
            guid = tonumber(iGuid),
            kills = tonumber(iKills), level = tonumber(iLevel), spent = tonumber(iSpent),
        }
        return true
    end

    -- прочее протокольное - просто прячем
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
-- Тултипы предметов и Alt+клик
-- ---------------------------------------------------------------------------

-- Отложенные запросы list / верификация локального кэша
local updater = CreateFrame("Frame")
updater:SetScript("OnUpdate", function()
    if listAt and GetTime() >= listAt then
        listAt = nil
        RequestList()
    end
    if syncAt and GetTime() >= syncAt then
        syncAt = nil
        SendCmd(".itemtalent sync " .. lastHash)
    end
end)

-- Строка состояния пробуждения в тултипе надетого предмета
hooksecurefunc(GameTooltip, "SetInventoryItem", function(tip, unit, slot)
    if unit ~= "player" then return end
    local st = invCache[slot]
    if not st then return end
    if st.spent > 0 then
        tip:AddLine(string.format("Пробуждён: %d из 5", st.spent), 1.0, 0.82, 0.0)
        if (st.level or 0) > st.spent then
            tip:AddLine("Доступен новый уровень пробуждения!", 0.55, 0.95, 0.35)
        end
    elseif (st.level or 0) > 0 then
        tip:AddLine("Готов к пробуждению!", 0.55, 0.95, 0.35)
    else
        tip:AddLine(string.format("Пробуждение: убийств %d", st.kills), 0.6, 0.62, 0.7)
    end
    tip:Show()
end)

-- Alt+ЛКМ по предмету: модифицированные клики проходят через OnModifiedClick,
-- который ловится hooksecurefunc (Ctrl занят примеркой, Shift - ссылкой/стаком)
hooksecurefunc("PaperDollItemSlotButton_OnModifiedClick", function(self, button)
    if button == "LeftButton" and IsAltKeyDown() then
        if not f:IsShown() then f:Show() end
        UpdateSlotButtons()
        SelectSlot(self:GetID())
    end
end)
hooksecurefunc("ContainerFrameItemButton_OnModifiedClick", function(self, button)
    if button == "LeftButton" and IsAltKeyDown() then
        Msg("Пробудить можно только надетый предмет: наденьте его и Alt+клик по слоту на персонаже.")
    end
end)

-- ---------------------------------------------------------------------------
-- Кнопка на окне персонажа (рядом с кнопкой трансмогрификации)
-- ---------------------------------------------------------------------------

-- Боковая вкладка на правом ребре окна персонажа (стиль вкладок книги
-- заклинаний). Вкладка TransmogUI стоит выше на y=-65 — держать отступы в паре.
local charBtn = CreateFrame("Button", "ItemTalentUICharButton", CharacterFrame)
charBtn:SetWidth(32)
charBtn:SetHeight(32)
charBtn:SetPoint("TOPLEFT", CharacterFrame, "TOPRIGHT", -33, -114)
charBtn:SetFrameLevel(CharacterFrame:GetFrameLevel() + 5)
charBtn:SetNormalTexture("Interface\\Icons\\spell_arcane_studentofmagic")
charBtn:GetNormalTexture():SetTexCoord(0.07, 0.93, 0.07, 0.93)
charBtn:SetHighlightTexture("Interface\\Buttons\\ButtonHilight-Square", "ADD")

local charBtnBg = charBtn:CreateTexture(nil, "BACKGROUND")
charBtnBg:SetTexture("Interface\\SpellBook\\SpellBook-SkillLineTab")
charBtnBg:SetWidth(64)
charBtnBg:SetHeight(64)
charBtnBg:SetPoint("TOPLEFT", charBtn, "TOPLEFT", -3, 11)

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
ev:RegisterEvent("PLAYER_ENTERING_WORLD")
ev:RegisterEvent("UNIT_INVENTORY_CHANGED")
ev:RegisterEvent("PLAYER_LEAVING_WORLD") -- логаут/релоад: форс-сохранение кэша (обходит троттлинг)
ev:SetScript("OnEvent", function(self, event, arg1)
    if event == "PLAYER_LEAVING_WORLD" then
        SaveCache(true)
        return
    end
    if event == "PLAYER_LOGIN" then
        ItemTalentUIDB = ItemTalentUIDB or {}
        if ItemTalentUIDB.pos then
            f:ClearAllPoints()
            f:SetPoint(ItemTalentUIDB.pos.point, UIParent, ItemTalentUIDB.pos.relPoint,
                ItemTalentUIDB.pos.x, ItemTalentUIDB.pos.y)
        end
        RenderEmpty()
        Msg("загружен. Кнопка на окне персонажа, /itu или Alt+клик по надетому предмету.")
    elseif event == "PLAYER_ENTERING_WORLD" then
        -- Только ОДИН раз за сессию: PLAYER_ENTERING_WORLD бьёт на каждой
        -- загрузке (телепорт/данж), а восстановить кэш и убийства достаточно
        -- единожды - дальше kills обновляются при открытии панели.
        if didInitialSync then return end
        didInitialSync = true
        charKey = charKey or (GetRealmName() .. "-" .. UnitName("player"))
        local saved = ItemTalentUIDB.chars and ItemTalentUIDB.chars[charKey]
        -- Требуем saved.trees: старый формат (v<=0.4) без деревьев/guid не даёт
        -- панельному кэшу ключей - для него уходим в полный list (ниже).
        if saved and saved.hash and saved.inv and saved.trees then
            -- Тултипы И деревья талантов сразу из локального кэша; сервер
            -- подтвердит хешем (совпало - только kills, панель без запросов;
            -- нет - полный list + сброс деревьев как устаревших).
            invCache = {}
            for slot, st in pairs(saved.inv) do
                invCache[slot] = { guid = st.guid, kills = st.kills,
                    level = st.level, spent = st.spent }
            end
            infoCache = saved.trees or {}
            lastHash = saved.hash
            syncPending = true
            syncAt = GetTime() + 8
        else
            listAt = GetTime() + 8 -- прогреть кэш тултипов после входа в мир
        end
    elseif event == "UNIT_INVENTORY_CHANGED" and arg1 == "player" then
        -- Фоновый list на смену экипировки убран: убийства обновляются
        -- при открытии панели. Пока панель открыта - только перерисовка
        -- слотов и точечный info выбранного (смена вещи - действие игрока).
        if f:IsShown() then
            UpdateSlotButtons()
            local now = GetTime()
            if now - invChangedAt > 1 then
                invChangedAt = now
                if selectedInv and GetInventoryItemLink("player", selectedInv) then
                    Refresh()
                end
            end
        end
    end
end)

SLASH_ITEMTALENTUI1 = "/itu"
SLASH_ITEMTALENTUI2 = "/itemtalent"
SlashCmdList["ITEMTALENTUI"] = function(arg)
    if arg == "debug" then
        ItemTalentUIDB = ItemTalentUIDB or {}
        ItemTalentUIDB.debug = not ItemTalentUIDB.debug
        Msg("дебаг " .. (ItemTalentUIDB.debug and "ВКЛ (клики покажут кэш/сервер)" or "выкл"))
        return
    end
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
