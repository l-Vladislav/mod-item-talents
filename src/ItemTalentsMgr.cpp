/*
 * mod-item-talents: реализация менеджера.
 *
 * Применение статов зеркалит Player::_ApplyItemBonuses (Player.cpp ~6604):
 * те же HandleStatFlatModifier / ApplyRatingMod / Apply*Bonus вызовы, что
 * ядро делает для соответствующих ITEM_MOD_*.
 */

#include "ItemTalentsMgr.h"
#include "Config.h"
#include "Containers.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Player.h"
#include "Random.h"
#include "SharedDefines.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Tokenize.h"
#include "Unit.h"
// Определение плейербота - тот же механизм, что в mod-ollama-chat
// (modules/mod-playerbots подключается через общие include-директории modules).
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include <cmath>
#include <numeric>

using ItemTalents::ItemState;
using ItemTalents::RollSlot;
using ItemTalents::TalentDef;
using ItemTalents::MAX_MENU_CHOICES;
using ItemTalents::MAX_ROWS;
using ItemTalents::MAX_SLOTS;
using ItemTalents::NUM_QUALITIES;

namespace
{
    bool WorldTableExists(char const* name)
    {
        return static_cast<bool>(WorldDatabase.Query(
            "SELECT 1 FROM information_schema.tables "
            "WHERE table_schema = DATABASE() AND table_name = '{}' LIMIT 1", name));
    }

    bool CharTableExists(char const* name)
    {
        return static_cast<bool>(CharacterDatabase.Query(
            "SELECT 1 FROM information_schema.tables "
            "WHERE table_schema = DATABASE() AND table_name = '{}' LIMIT 1", name));
    }
}

ItemTalentsMgr* ItemTalentsMgr::instance()
{
    static ItemTalentsMgr instance;
    return &instance;
}

void ItemTalentsMgr::LoadConfig()
{
    _enable = sConfigMgr->GetOption<bool>("ItemTalents.Enable", true);
    _maxImplementedRow = uint8(sConfigMgr->GetOption<uint32>("ItemTalents.MaxImplementedRow", 2));
    _masterRange = sConfigMgr->GetOption<float>("ItemTalents.MasterRange", 10.0f);
    _eliteMultiplier = sConfigMgr->GetOption<uint32>("ItemTalents.EliteMultiplier", 1);
    _ignoreBots = sConfigMgr->GetOption<bool>("ItemTalents.IgnoreBots", true);

    // Сегменты убийств за 1..5-е очко -> кумулятивные пороги (50,200,600,1600,4100)
    _cumThresholds.clear();
    std::string const thresholds =
        sConfigMgr->GetOption<std::string>("ItemTalents.PointThresholds", "50,150,400,1000,2500");
    uint32 cumulative = 0;
    for (std::string_view token : Acore::Tokenize(thresholds, ',', false))
        if (Optional<uint32> segment = Acore::StringTo<uint32>(token))
        {
            cumulative += *segment;
            _cumThresholds.push_back(cumulative);
        }

    if (_cumThresholds.size() != MAX_ROWS)
        LOG_WARN("module", "mod-item-talents: ItemTalents.PointThresholds has {} valid segments, "
            "expected {}. Points capped accordingly.", _cumThresholds.size(), uint32(MAX_ROWS));

    _masterEntries.clear();
    std::string const masters = sConfigMgr->GetOption<std::string>("ItemTalents.MasterEntries",
        "2704,11865,11866,11867,11868,11869,11870,13084,16621,16773,17005");
    for (std::string_view token : Acore::Tokenize(masters, ',', false))
        if (Optional<uint32> entry = Acore::StringTo<uint32>(token))
            _masterEntries.push_back(*entry);

    // Качество ролла: веса выпадения (Обычный/Отличный/Совершенный) и
    // множители значения перка. Кол-во != 3 -> дефолты.
    _qualityChances = { 70.0, 25.0, 5.0 };
    std::string const weights =
        sConfigMgr->GetOption<std::string>("ItemTalents.QualityWeights", "70,25,5");
    std::vector<double> parsedWeights;
    for (std::string_view token : Acore::Tokenize(weights, ',', false))
        if (Optional<double> weight = Acore::StringTo<double>(token))
            parsedWeights.push_back(*weight);
    if (parsedWeights.size() == NUM_QUALITIES
        && std::accumulate(parsedWeights.begin(), parsedWeights.end(), 0.0) > 0.0)
        std::copy(parsedWeights.begin(), parsedWeights.end(), _qualityChances.begin());
    else
        LOG_WARN("module", "mod-item-talents: ItemTalents.QualityWeights '{}' is invalid "
            "(need {} values with positive sum). Using defaults 70,25,5.", weights,
            uint32(NUM_QUALITIES));

    _qualityMults = { 1.0f, 1.25f, 1.5f };
    std::string const mults =
        sConfigMgr->GetOption<std::string>("ItemTalents.QualityMults", "1.0,1.25,1.5");
    std::vector<float> parsedMults;
    for (std::string_view token : Acore::Tokenize(mults, ',', false))
        if (Optional<float> mult = Acore::StringTo<float>(token))
            parsedMults.push_back(*mult);
    if (parsedMults.size() == NUM_QUALITIES)
        std::copy(parsedMults.begin(), parsedMults.end(), _qualityMults.begin());
    else
        LOG_WARN("module", "mod-item-talents: ItemTalents.QualityMults '{}' has {} valid "
            "values, expected {}. Using defaults 1.0,1.25,1.5.", mults, parsedMults.size(),
            uint32(NUM_QUALITIES));

    LOG_INFO("module", "mod-item-talents: enable={} maxRow={} masters={} range={:.1f} "
        "ignoreBots={}", _enable, _maxImplementedRow, _masterEntries.size(), _masterRange,
        _ignoreBots);
}

void ItemTalentsMgr::LoadDefinitions()
{
    _defs.clear();
    _menus.clear();
    _defsLoaded = false;

    if (!_enable)
        return;

    // Известная грабля проекта: SELECT из несуществующей таблицы абортит
    // worldserver - сперва проверяем через information_schema.
    if (!WorldTableExists("item_talent_def"))
    {
        LOG_WARN("module", "mod-item-talents: acore_world.item_talent_def is missing; "
            "module disabled until SQL is applied.");
        _enable = false;
        return;
    }

    if (!CharTableExists("item_talents"))
    {
        LOG_WARN("module", "mod-item-talents: acore_characters.item_talents is missing; "
            "module disabled until SQL is applied.");
        _enable = false;
        return;
    }

    if (!CharTableExists("item_talent_rolls"))
    {
        LOG_WARN("module", "mod-item-talents: acore_characters.item_talent_rolls is missing; "
            "module disabled until SQL is applied.");
        _enable = false;
        return;
    }

    QueryResult result = WorldDatabase.Query(
        "SELECT pool, `row`, choice, name_ru, desc_ru, effect, base, per_ilvl FROM item_talent_def");
    if (!result)
    {
        LOG_WARN("module", "mod-item-talents: item_talent_def is empty; module disabled.");
        _enable = false;
        return;
    }

    uint32 count = 0;
    do
    {
        Field* fields = result->Fetch();
        std::string const pool = fields[0].Get<std::string>();
        int8 const row = fields[1].Get<int8>();
        int8 const choice = fields[2].Get<int8>();
        if (pool.empty() || row < 1 || row > int8(MAX_ROWS) || choice < 1
            || choice > int8(MAX_MENU_CHOICES))
        {
            LOG_WARN("module", "mod-item-talents: skipped bad item_talent_def row "
                "(pool '{}', row {}, choice {}).", pool, row, choice);
            continue;
        }

        TalentDef def;
        def.nameRu = fields[3].Get<std::string>();
        def.descRu = fields[4].Get<std::string>();
        def.effect = fields[5].Get<std::string>();
        def.base = fields[6].Get<float>();
        def.perIlvl = fields[7].Get<float>();
        _defs[MakeKey(pool[0], uint8(row), uint8(choice))] = std::move(def);
        _menus[MakeKey(pool[0], uint8(row), 0)].push_back(uint8(choice));
        ++count;
    } while (result->NextRow());

    _defsLoaded = true;
    LOG_INFO("module", "mod-item-talents: loaded {} talent definitions in {} row menus.",
        count, _menus.size());
}

bool ItemTalentsMgr::ShouldIgnorePlayer(Player* player) const
{
    if (!_ignoreBots || !player)
        return false;

    return PlayerbotsMgr::instance().GetPlayerbotAI(player) != nullptr;
}

// ---------------------------------------------------------------------------
// Статика по предмету (DESIGN §4)
// ---------------------------------------------------------------------------

std::optional<char> ItemTalentsMgr::GetPool(uint32 itemClass, uint32 itemSubClass)
{
    if (itemClass == ITEM_CLASS_ARMOR)
        switch (itemSubClass)
        {
            case ITEM_SUBCLASS_ARMOR_CLOTH:   return 'A';
            case ITEM_SUBCLASS_ARMOR_LEATHER: return 'B';
            case ITEM_SUBCLASS_ARMOR_MAIL:    return 'C';
            case ITEM_SUBCLASS_ARMOR_PLATE:   return 'D';
            case ITEM_SUBCLASS_ARMOR_SHIELD:  return 'E';
            default:                          return std::nullopt;
        }

    if (itemClass == ITEM_CLASS_WEAPON)
        switch (itemSubClass)
        {
            case ITEM_SUBCLASS_WEAPON_AXE:
            case ITEM_SUBCLASS_WEAPON_AXE2:
            case ITEM_SUBCLASS_WEAPON_MACE:
            case ITEM_SUBCLASS_WEAPON_MACE2:
            case ITEM_SUBCLASS_WEAPON_POLEARM:
            case ITEM_SUBCLASS_WEAPON_SWORD:
            case ITEM_SUBCLASS_WEAPON_SWORD2:
            case ITEM_SUBCLASS_WEAPON_FIST:
            case ITEM_SUBCLASS_WEAPON_DAGGER:   return 'F';
            case ITEM_SUBCLASS_WEAPON_BOW:
            case ITEM_SUBCLASS_WEAPON_GUN:
            case ITEM_SUBCLASS_WEAPON_CROSSBOW: return 'G';
            case ITEM_SUBCLASS_WEAPON_STAFF:
            case ITEM_SUBCLASS_WEAPON_WAND:     return 'H';
            default:                            return std::nullopt;
        }

    return std::nullopt;
}

uint8 ItemTalentsMgr::RowsOpenForQuality(uint32 quality)
{
    switch (quality)
    {
        case ITEM_QUALITY_NORMAL:    return 1; // белый
        case ITEM_QUALITY_UNCOMMON:  return 2; // зелёный
        case ITEM_QUALITY_RARE:      return 3; // синий
        case ITEM_QUALITY_EPIC:      return 4;
        case ITEM_QUALITY_LEGENDARY:
        case ITEM_QUALITY_ARTIFACT:  return 5;
        default:                     return 0; // серое (0) и наследие (7) - вне системы
    }
}

bool ItemTalentsMgr::IsEligibleItem(ItemTemplate const* proto)
{
    if (!proto)
        return false;

    if (proto->InventoryType == INVTYPE_BODY || proto->InventoryType == INVTYPE_TABARD)
        return false;

    if (!RowsOpenForQuality(proto->Quality))
        return false;

    return GetPool(proto->Class, proto->SubClass).has_value();
}

TalentDef const* ItemTalentsMgr::GetDef(char pool, uint8 row, uint8 choice) const
{
    auto itr = _defs.find(MakeKey(pool, row, choice));
    return itr != _defs.end() ? &itr->second : nullptr;
}

std::vector<uint8> const* ItemTalentsMgr::GetMenu(char pool, uint8 row) const
{
    auto itr = _menus.find(MakeKey(pool, row, 0));
    return itr != _menus.end() ? &itr->second : nullptr;
}

int32 ItemTalentsMgr::CalcValue(TalentDef const& def, uint32 itemLevel, uint8 quality) const
{
    // База: флэт скейлится от ilvl (минимум 1), проценты фиксированы в base.
    int32 base;
    if (def.perIlvl > 0.0f)
        base = std::max(1, int32(std::ceil(def.base + def.perIlvl * float(itemLevel))));
    else
        base = int32(std::ceil(def.base));

    uint8 const q = std::min<uint8>(quality, NUM_QUALITIES - 1);

    // Исключение (решение 2026-07-06): износ - ровные ступени 50/65/80,
    // "чтобы выглядело ровно", на геймплей влияет слабо.
    if (def.effect == "DURA_SAVE")
        return base + 15 * int32(q);

    // Каждая ступень качества даёт МИНИМУМ +1 к предыдущей (решение
    // 2026-07-06), иначе множитель на малых базах не ощущается:
    // значение(q) = max(значение(q-1) + 1, ceil(база * mult[q])).
    int32 value = base;
    for (uint8 tier = 1; tier <= q; ++tier)
        value = std::max(value + 1, int32(std::ceil(float(base) * _qualityMults[tier])));

    return value;
}

// ---------------------------------------------------------------------------
// Кэш состояния
// ---------------------------------------------------------------------------

void ItemTalentsMgr::LoadPlayerState(Player* player)
{
    if (!IsEnabled() || !player)
        return;

    ObjectGuid::LowType const ownerGuid = player->GetGUID().GetCounter();
    if (_states.contains(ownerGuid))
        return; // уже загружено

    OwnerStates& states = _states[ownerGuid]; // помечает владельца загруженным даже при 0 строк

    QueryResult result = CharacterDatabase.Query(
        "SELECT item_guid, row1, row2, row3, row4, row5, kills FROM item_talents "
        "WHERE owner_guid = {}", ownerGuid);
    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();
        ItemState& state = states[fields[0].Get<uint32>()];
        for (uint8 i = 0; i < MAX_ROWS; ++i)
            state.rows[i] = fields[1 + i].Get<uint8>();
        state.kills = fields[6].Get<uint32>();
        state.dirtyKills = 0;
    } while (result->NextRow());

    // Роллы тех же предметов вторым запросом. JOIN на item_talents: владельца
    // знает только она; предмет с роллами, но без строки item_talents, сюда
    // не попадёт - его добирает точечный путь EnsureState.
    QueryResult rolls = CharacterDatabase.Query(
        "SELECT r.item_guid, r.`row`, r.slot, r.choice, r.quality FROM item_talent_rolls r "
        "JOIN item_talents t ON t.item_guid = r.item_guid WHERE t.owner_guid = {}", ownerGuid);
    if (!rolls)
        return;

    do
    {
        Field* fields = rolls->Fetch();
        uint8 const row = fields[1].Get<uint8>();
        uint8 const slot = fields[2].Get<uint8>();
        if (row < 1 || row > MAX_ROWS || slot < 1 || slot > MAX_SLOTS)
            continue;

        RollSlot& roll = states[fields[0].Get<uint32>()].rolls[row - 1][slot - 1];
        roll.choice = fields[3].Get<uint8>();
        roll.quality = fields[4].Get<uint8>();
    } while (rolls->NextRow());
}

void ItemTalentsMgr::UnloadPlayerState(Player* player)
{
    if (player)
        _states.erase(player->GetGUID().GetCounter());
}

ItemState* ItemTalentsMgr::GetState(ObjectGuid::LowType ownerGuid, ObjectGuid::LowType itemGuid)
{
    auto ownerItr = _states.find(ownerGuid);
    if (ownerItr == _states.end())
        return nullptr;

    auto itemItr = ownerItr->second.find(itemGuid);
    return itemItr != ownerItr->second.end() ? &itemItr->second : nullptr;
}

ItemState& ItemTalentsMgr::EnsureState(Player* player, Item* item)
{
    ObjectGuid::LowType const ownerGuid = player->GetGUID().GetCounter();
    if (!_states.contains(ownerGuid))
        LoadPlayerState(player);

    OwnerStates& states = _states[ownerGuid];
    ObjectGuid::LowType const itemGuid = item->GetGUID().GetCounter();
    auto itr = states.find(itemGuid);
    if (itr == states.end())
    {
        // Мимо владельческой выборки: предмет мог прийти от другого игрока
        // (owner_guid в БД обновляется только при INSERT) - точечный SELECT.
        ItemState state;
        if (QueryResult result = CharacterDatabase.Query(
            "SELECT row1, row2, row3, row4, row5, kills FROM item_talents WHERE item_guid = {}",
            itemGuid))
        {
            Field* fields = result->Fetch();
            for (uint8 i = 0; i < MAX_ROWS; ++i)
                state.rows[i] = fields[i].Get<uint8>();
            state.kills = fields[5].Get<uint32>();
        }

        if (QueryResult rolls = CharacterDatabase.Query(
            "SELECT `row`, slot, choice, quality FROM item_talent_rolls WHERE item_guid = {}",
            itemGuid))
            do
            {
                Field* fields = rolls->Fetch();
                uint8 const row = fields[0].Get<uint8>();
                uint8 const slot = fields[1].Get<uint8>();
                if (row < 1 || row > MAX_ROWS || slot < 1 || slot > MAX_SLOTS)
                    continue;

                RollSlot& roll = state.rolls[row - 1][slot - 1];
                roll.choice = fields[2].Get<uint8>();
                roll.quality = fields[3].Get<uint8>();
            } while (rolls->NextRow());

        itr = states.emplace(itemGuid, state).first;
    }

    // Ленивый ролл слотов (DESIGN "Роллы перков и качество"): только реальным
    // игрокам - предметы ботов не роллим.
    if (!ShouldIgnorePlayer(player))
        EnsureRolled(player, item);

    return itr->second;
}

void ItemTalentsMgr::EnsureRolled(Player* player, Item* item)
{
    if (!IsEnabled() || !player || !item)
        return;

    // Вызывается из EnsureState: состояние уже в кэше.
    ItemState* state = GetState(player->GetGUID().GetCounter(), item->GetGUID().GetCounter());
    if (!state)
        return;

    // Любой существующий ролл = предмет уже роллен (ряд 1 в сиде есть всегда,
    // но проверяем все ряды - защита от урезанного сида).
    for (auto const& rowRolls : state->rolls)
        for (RollSlot const& roll : rowRolls)
            if (roll.choice)
                return;

    ItemTemplate const* proto = item->GetTemplate();
    if (!proto)
        return;

    std::optional<char> pool = GetPool(proto->Class, proto->SubClass);
    if (!pool)
        return;

    std::string values;
    for (uint8 row = 1; row <= MAX_ROWS; ++row)
    {
        std::vector<uint8> const* menu = GetMenu(*pool, row);
        if (!menu || menu->empty())
            continue;

        // Миграция старых данных: rowN раньше хранил choice 1..3 - слот с тем
        // же номером закрепляет старый choice с качеством 0, применённые статы
        // не меняются (rowN как СЛОТ указывает туда же).
        uint8 const legacy = state->rows[row - 1] <= MAX_SLOTS ? state->rows[row - 1] : 0;

        std::vector<uint8> options = *menu;
        if (legacy)
            std::erase(options, legacy);
        Acore::Containers::RandomShuffle(options);

        std::size_t next = 0;
        for (uint8 slot = 1; slot <= MAX_SLOTS; ++slot)
        {
            RollSlot& roll = state->rolls[row - 1][slot - 1];
            if (legacy && slot == legacy)
            {
                roll.choice = legacy;
                roll.quality = 0;
            }
            else if (next < options.size())
            {
                roll.choice = options[next++];
                roll.quality = RollQuality();
            }
            else
                continue; // вариантов в меню меньше, чем слотов

            if (!values.empty())
                values += ',';
            values += Acore::StringFormat("({},{},{},{},{})", item->GetGUID().GetCounter(),
                row, slot, roll.choice, roll.quality);
        }
    }

    if (values.empty())
        return;

    // Sync-запись допустима: ролл - разовое событие на предмет.
    CharacterDatabase.DirectExecute(
        "INSERT INTO item_talent_rolls (item_guid, `row`, slot, choice, quality) VALUES "
        + values + " ON DUPLICATE KEY UPDATE choice = VALUES(choice), "
        "quality = VALUES(quality)");
}

uint8 ItemTalentsMgr::RollQuality() const
{
    return uint8(urandweighted(NUM_QUALITIES, _qualityChances.data()));
}

// ---------------------------------------------------------------------------
// Очки
// ---------------------------------------------------------------------------

uint32 ItemTalentsMgr::EarnedPoints(uint32 kills) const
{
    uint32 earned = 0;
    for (uint32 threshold : _cumThresholds)
        if (kills >= threshold)
            ++earned;

    return earned;
}

uint32 ItemTalentsMgr::SpentPoints(ItemState const& state)
{
    uint32 spent = 0;
    for (uint8 slot : state.rows)
        if (slot)
            ++spent;

    return spent;
}

uint32 ItemTalentsMgr::FreePoints(ItemState const& state) const
{
    uint32 const earned = EarnedPoints(state.kills);
    uint32 const spent = SpentPoints(state);
    return earned > spent ? earned - spent : 0;
}

uint32 ItemTalentsMgr::NextPointNeed(uint32 kills) const
{
    for (uint32 threshold : _cumThresholds)
        if (kills < threshold)
            return threshold;

    return 0; // все очки заработаны
}

// ---------------------------------------------------------------------------
// Опыт предмета
// ---------------------------------------------------------------------------

void ItemTalentsMgr::AddKill(Player* player)
{
    if (!IsEnabled() || !player)
        return;

    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item || !IsEligibleItem(item->GetTemplate()))
            continue;

        // _eliteMultiplier - задел на будущее (DESIGN: пока всегда +1)
        ItemState& state = EnsureState(player, item);
        ++state.kills;
        ++state.dirtyKills;
    }
}

void ItemTalentsMgr::FlushKills(Player* player)
{
    if (!player)
        return;

    ObjectGuid::LowType const ownerGuid = player->GetGUID().GetCounter();
    auto ownerItr = _states.find(ownerGuid);
    if (ownerItr == _states.end())
        return;

    std::string values;
    for (auto& [itemGuid, state] : ownerItr->second)
    {
        if (!state.dirtyKills)
            continue;

        if (!values.empty())
            values += ',';
        values += Acore::StringFormat("({},{},{})", itemGuid, ownerGuid, state.dirtyKills);
        state.dirtyKills = 0;
    }

    if (values.empty())
        return;

    // owner_guid обновляем тоже: предмет могли передать до привязки
    CharacterDatabase.Execute(
        "INSERT INTO item_talents (item_guid, owner_guid, kills) VALUES " + values +
        " ON DUPLICATE KEY UPDATE kills = kills + VALUES(kills), "
        "owner_guid = VALUES(owner_guid)");
}

// ---------------------------------------------------------------------------
// Применение статов
// ---------------------------------------------------------------------------

void ItemTalentsMgr::ApplyTalent(Player* player, Item* item, uint8 row, uint8 slot, bool apply)
{
    if (row < 1 || row > MAX_ROWS || slot < 1 || slot > MAX_SLOTS)
        return;

    ItemState* state = GetState(player->GetGUID().GetCounter(), item->GetGUID().GetCounter());
    if (!state)
        return;

    RollSlot const& roll = state->rolls[row - 1][slot - 1];
    if (!roll.choice)
        return;

    ItemTemplate const* proto = item->GetTemplate();
    if (!proto)
        return;

    std::optional<char> pool = GetPool(proto->Class, proto->SubClass);
    if (!pool)
        return;

    TalentDef const* def = GetDef(*pool, row, roll.choice);
    if (!def)
        return;

    ApplyEffect(player, def->effect, CalcValue(*def, proto->ItemLevel, roll.quality), apply);
}

void ItemTalentsMgr::ApplyAllTalents(Player* player, Item* item, bool apply)
{
    if (!IsEnabled() || !player || !item)
        return;

    if (!IsEligibleItem(item->GetTemplate()))
        return;

    ItemState* state = GetState(player->GetGUID().GetCounter(), item->GetGUID().GetCounter());
    if (!state)
    {
        // Снятие без закэшированного состояния = ничего не применялось.
        if (!apply)
            return;

        // Ботов не грузим из БД вовсе (сотни бот-логинов; их предметы вне системы).
        if (ShouldIgnorePlayer(player))
            return;

        state = &EnsureState(player, item);
    }

    for (uint8 row = 1; row <= MAX_ROWS; ++row)
        if (uint8 slot = state->rows[row - 1])
            ApplyTalent(player, item, row, slot, apply);
}

// Зеркалит Player::_ApplyItemBonuses для соответствующих ITEM_MOD_*.
void ItemTalentsMgr::ApplyEffect(Player* player, std::string const& effect, int32 value,
    bool apply) const
{
    float const fval = float(value);

    if (effect == "STAT_STA") // как ITEM_MOD_STAMINA
    {
        player->HandleStatFlatModifier(UNIT_MOD_STAT_STAMINA, BASE_VALUE, fval, apply);
        player->UpdateStatBuffMod(STAT_STAMINA);
    }
    else if (effect == "STAT_AGI") // как ITEM_MOD_AGILITY
    {
        player->HandleStatFlatModifier(UNIT_MOD_STAT_AGILITY, BASE_VALUE, fval, apply);
        player->UpdateStatBuffMod(STAT_AGILITY);
    }
    else if (effect == "STAT_STR") // как ITEM_MOD_STRENGTH
    {
        player->HandleStatFlatModifier(UNIT_MOD_STAT_STRENGTH, BASE_VALUE, fval, apply);
        player->UpdateStatBuffMod(STAT_STRENGTH);
    }
    else if (effect == "STAT_INT") // как ITEM_MOD_INTELLECT
    {
        player->HandleStatFlatModifier(UNIT_MOD_STAT_INTELLECT, BASE_VALUE, fval, apply);
        player->UpdateStatBuffMod(STAT_INTELLECT);
    }
    else if (effect == "STAT_SPI") // как ITEM_MOD_SPIRIT
    {
        player->HandleStatFlatModifier(UNIT_MOD_STAT_SPIRIT, BASE_VALUE, fval, apply);
        player->UpdateStatBuffMod(STAT_SPIRIT);
    }
    else if (effect == "ATTACK_POWER") // как ITEM_MOD_ATTACK_POWER (ближняя И дальняя)
    {
        player->HandleStatFlatModifier(UNIT_MOD_ATTACK_POWER, TOTAL_VALUE, fval, apply);
        player->HandleStatFlatModifier(UNIT_MOD_ATTACK_POWER_RANGED, TOTAL_VALUE, fval, apply);
    }
    else if (effect == "SPELL_POWER") // как ITEM_MOD_SPELL_POWER
        player->ApplySpellPowerBonus(value, apply);
    else if (effect == "RESIST_ALL") // как proto->HolyRes..ArcaneRes
    {
        for (uint8 school = SPELL_SCHOOL_HOLY; school < MAX_SPELL_SCHOOL; ++school)
            player->HandleStatFlatModifier(UnitMods(UNIT_MOD_RESISTANCE_START + school),
                BASE_VALUE, fval, apply);
    }
    else if (effect == "BLOCK_VALUE") // как ITEM_MOD_BLOCK_VALUE
        player->HandleBaseModFlatValue(SHIELD_BLOCK_VALUE, fval, apply);
    else if (effect == "RATING_CRIT") // как ITEM_MOD_CRIT_RATING
    {
        player->ApplyRatingMod(CR_CRIT_MELEE, value, apply);
        player->ApplyRatingMod(CR_CRIT_RANGED, value, apply);
        player->ApplyRatingMod(CR_CRIT_SPELL, value, apply);
    }
    else if (effect == "RATING_HASTE") // как ITEM_MOD_HASTE_RATING
    {
        player->ApplyRatingMod(CR_HASTE_MELEE, value, apply);
        player->ApplyRatingMod(CR_HASTE_RANGED, value, apply);
        player->ApplyRatingMod(CR_HASTE_SPELL, value, apply);
    }
    else if (effect == "RATING_HIT") // как ITEM_MOD_HIT_RATING
    {
        player->ApplyRatingMod(CR_HIT_MELEE, value, apply);
        player->ApplyRatingMod(CR_HIT_RANGED, value, apply);
        player->ApplyRatingMod(CR_HIT_SPELL, value, apply);
    }
    else if (effect == "RATING_DODGE") // как ITEM_MOD_DODGE_RATING
        player->ApplyRatingMod(CR_DODGE, value, apply);
    else if (effect == "RATING_DEFENSE") // как ITEM_MOD_DEFENSE_SKILL_RATING
        player->ApplyRatingMod(CR_DEFENSE_SKILL, value, apply);
    else if (effect == "RATING_PARRY") // как ITEM_MOD_PARRY_RATING
        player->ApplyRatingMod(CR_PARRY, value, apply);
    else if (effect == "RATING_BLOCK") // как ITEM_MOD_BLOCK_RATING
        player->ApplyRatingMod(CR_BLOCK, value, apply);
    else if (effect == "RATING_ARMOR_PEN") // как ITEM_MOD_ARMOR_PENETRATION_RATING
        player->ApplyRatingMod(CR_ARMOR_PENETRATION, value, apply);
    else if (effect == "SPELL_PEN") // как ITEM_MOD_SPELL_PENETRATION
        player->ApplySpellPenetrationBonus(value, apply);
    else if (effect == "MP5") // как ITEM_MOD_MANA_REGENERATION
        player->ApplyManaRegenBonus(value, apply);
    else if (effect == "DURA_SAVE" || effect == "MOVE_SPEED_PCT" || effect == "HEAL_TAKEN_PCT"
        || effect == "PHYS_TAKEN_PCT" || effect == "NEMESIS_DMG_PCT"
        || effect == "FAMILIAR_ALL_STATS" || effect == "GOLD_XP_PCT")
    {
        // TODO(phase-next): утилити/синергия рядов 3-4 (в v1 выбор рядов 3+
        // всё равно запрещён конфигом ItemTalents.MaxImplementedRow).
        LOG_DEBUG("module", "mod-item-talents: effect {} (value {}) is a v1 stub, no-op.",
            effect, value);
    }
    else
        LOG_WARN("module", "mod-item-talents: unknown effect '{}' in item_talent_def.", effect);
}

// ---------------------------------------------------------------------------
// Выбор / сброс
// ---------------------------------------------------------------------------

void ItemTalentsMgr::SaveChoice(Player* player, Item* item, uint8 row, uint8 slot)
{
    // rowN хранит выбранный СЛОТ 1..3 (choice лежит в item_talent_rolls)
    ItemState& state = EnsureState(player, item);
    state.rows[row - 1] = slot;

    // kills в БД = текущее значение минус несброшенная дельта (её допишет FlushKills)
    uint32 const dbKills = state.kills - state.dirtyKills;
    CharacterDatabase.DirectExecute(
        "INSERT INTO item_talents (item_guid, owner_guid, row1, row2, row3, row4, row5, kills) "
        "VALUES ({}, {}, {}, {}, {}, {}, {}, {}) ON DUPLICATE KEY UPDATE row{} = {}, "
        "owner_guid = VALUES(owner_guid)",
        item->GetGUID().GetCounter(), player->GetGUID().GetCounter(),
        state.rows[0], state.rows[1], state.rows[2], state.rows[3], state.rows[4],
        dbKills, row, slot);
}

void ItemTalentsMgr::ResetChoice(Player* player, Item* item, uint8 row)
{
    ItemState& state = EnsureState(player, item);
    state.rows[row - 1] = 0;

    CharacterDatabase.DirectExecute("UPDATE item_talents SET row{} = 0 WHERE item_guid = {}",
        row, item->GetGUID().GetCounter());
}

bool ItemTalentsMgr::IsNearMaster(Player* player) const
{
    for (uint32 entry : _masterEntries)
        if (player->FindNearestCreature(entry, _masterRange))
            return true;

    return false;
}
