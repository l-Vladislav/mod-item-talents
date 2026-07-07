/*
 * mod-item-talents: реализация менеджера.
 *
 * Применение статов зеркалит Player::_ApplyItemBonuses (Player.cpp ~6604):
 * те же HandleStatFlatModifier / ApplyRatingMod / Apply*Bonus вызовы, что
 * ядро делает для соответствующих ITEM_MOD_*.
 *
 * Ряды 3-4 (агрегируются по игроку в PlayerPerks, симметрично apply/unapply):
 *  - MOVE_SPEED_PCT / HEAL_TAKEN_PCT / PHYS_TAKEN_PCT: server-side ауры
 *    spell_dbc 108900-108914 (по 5 рангов 1..5%), на игроке держится один
 *    ранг = min(сумма, кап, 5). Аура 129 (MOD_SPEED_ALWAYS, как энчант
 *    сапог "Скороход": стакается со Спринтом мультипликативно; аура 31
 *    MOD_INCREASE_SPEED берётся ядром как max() и съедалась бы Спринтом,
 *    а ходьба (MOVE_WALK) в 3.3.5 положительными аурами не ускоряется
 *    вовсе - см. Unit::UpdateSpeed), 118 (MOD_HEALING_PCT),
 *    87 (MOD_DAMAGE_PERCENT_TAKEN, физ. школа, отрицательная).
 *  - DURA_SAVE: хук OnPlayerDurabilityPointsLoss - N% событий износа
 *    предмета пропускается (только сам предмет).
 *  - NEMESIS_DMG_PCT: UnitScript-хуки урона + кэш spawnId немезид из
 *    acore_characters.character_nemesis (лучший доступный признак; см.
 *    TODO у IsNemesisTarget).
 *  - FAMILIAR_ALL_STATS: флэт всех статов, пока на игроке владельческая
 *    аура фамильяра (103000-103099 / 104000-104099); пересчёт на
 *    UnitScript::OnAuraApply/OnAuraRemove и на equip-путях.
 *  - GOLD_XP_PCT: хуки OnPlayerBeforeLootMoney (золото с существ) и
 *    OnPlayerGiveXP (+1% за предмет).
 */

#include "ItemTalentsMgr.h"
#include "Chat.h"
#include "Config.h"
#include "Containers.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Random.h"
#include "SharedDefines.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Timer.h"
#include "Tokenize.h"
#include "Unit.h"
// Определение плейербота - тот же механизм, что в mod-ollama-chat
// (modules/mod-playerbots подключается через общие include-директории modules).
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include <algorithm>
#include <cmath>
#include <numeric>

using ItemTalents::ItemState;
using ItemTalents::PlayerPerks;
using ItemTalents::RollSlot;
using ItemTalents::TalentDef;
using ItemTalents::AURA_RANKS;
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

    bool WorldColumnExists(char const* table, char const* column)
    {
        return static_cast<bool>(WorldDatabase.Query(
            "SELECT 1 FROM information_schema.columns WHERE table_schema = DATABASE() "
            "AND table_name = '{}' AND column_name = '{}' LIMIT 1", table, column));
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
    _maxImplementedRow = uint8(sConfigMgr->GetOption<uint32>("ItemTalents.MaxImplementedRow", 4));
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

    // Ручные диапазоны entry для ряда 5 (стиль ahbot): "100-200,345841,..."
    auto parseRanges = [](std::string const& value, std::vector<std::pair<uint32, uint32>>& out)
    {
        out.clear();
        for (std::string_view token : Acore::Tokenize(value, ',', false))
        {
            size_t const dash = token.find('-');
            if (dash == std::string_view::npos)
            {
                if (Optional<uint32> single = Acore::StringTo<uint32>(token))
                    out.emplace_back(*single, *single);
            }
            else if (Optional<uint32> lo = Acore::StringTo<uint32>(token.substr(0, dash)))
                if (Optional<uint32> hi = Acore::StringTo<uint32>(token.substr(dash + 1)))
                    if (*lo <= *hi)
                        out.emplace_back(*lo, *hi);
        }
    };
    parseRanges(sConfigMgr->GetOption<std::string>("ItemTalents.Row5.AllowEntryRanges", ""),
        _row5Allow);
    parseRanges(sConfigMgr->GetOption<std::string>("ItemTalents.Row5.DenyEntryRanges", ""),
        _row5Deny);
    _baseEpicCache.clear(); // конфиг мог поменять вердикты

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

    // Капы стакинга рядов 3-4 (PERKS.md). Примечания:
    //  - аурные эффекты (бег/лечение/физ.) дополнительно ограничены числом
    //    рангов спеллов (AURA_RANKS = 5%): кап лечения 10% сейчас недостижим,
    //    для него нужны ещё 5 спеллов-рангов;
    //  - физ. кап 3 (не 1 из PERKS.md): единственный щит может роллнуть
    //    Совершенное качество = 3%, кап 1 обманывал бы тултип.
    _capMoveSpeedPct = sConfigMgr->GetOption<int32>("ItemTalents.Cap.MoveSpeedPct", 5);
    _capHealTakenPct = sConfigMgr->GetOption<int32>("ItemTalents.Cap.HealTakenPct", 10);
    _capPhysTakenPct = sConfigMgr->GetOption<int32>("ItemTalents.Cap.PhysTakenPct", 3);
    _capNemesisDmgPct = sConfigMgr->GetOption<int32>("ItemTalents.Cap.NemesisDmgPct", 15);
    _capGoldPct = sConfigMgr->GetOption<int32>("ItemTalents.Cap.GoldPct", 20);
    _capXpPct = sConfigMgr->GetOption<int32>("ItemTalents.Cap.XpPct", 10);

    _nemesisRefreshMs =
        sConfigMgr->GetOption<uint32>("ItemTalents.NemesisCacheRefreshSecs", 30) * IN_MILLISECONDS;

    // Владельческие ауры фамильяров (гача): "начало-конец" через запятую
    _familiarAuraRanges.clear();
    std::string const ranges = sConfigMgr->GetOption<std::string>(
        "ItemTalents.FamiliarAuraRanges", "103000-103099,104000-104099");
    for (std::string_view token : Acore::Tokenize(ranges, ',', false))
    {
        std::string_view::size_type const dash = token.find('-');
        if (dash == std::string_view::npos)
            continue;

        Optional<uint32> lo = Acore::StringTo<uint32>(token.substr(0, dash));
        Optional<uint32> hi = Acore::StringTo<uint32>(token.substr(dash + 1));
        if (lo && hi && *lo <= *hi)
            _familiarAuraRanges.emplace_back(*lo, *hi);
        else
            LOG_WARN("module", "mod-item-talents: bad range '{}' in "
                "ItemTalents.FamiliarAuraRanges.", std::string(token));
    }

    // Звуки пробуждённых предметов: "entry:soundId" через запятую.
    // Дефолт: классический эпик 873 "Посох Джордана" (двуручный посох) +
    // HumanMaleOfficialNPCGreetings (SoundEntries 5971). Играет ТОЛЬКО у
    // полностью пробуждённого оружия в основной руке (см. PlayItemSound).
    _itemSounds.clear();
    std::string const sounds =
        sConfigMgr->GetOption<std::string>("ItemTalents.ItemSounds", "873:5971");
    for (std::string_view token : Acore::Tokenize(sounds, ',', false))
    {
        std::string_view::size_type const colon = token.find(':');
        if (colon == std::string_view::npos)
            continue;

        Optional<uint32> entry = Acore::StringTo<uint32>(token.substr(0, colon));
        Optional<uint32> soundId = Acore::StringTo<uint32>(token.substr(colon + 1));
        if (entry && soundId && *entry && *soundId)
            _itemSounds[*entry] = *soundId;
        else
            LOG_WARN("module", "mod-item-talents: bad pair '{}' in ItemTalents.ItemSounds.",
                std::string(token));
    }

    _soundCooldownMs =
        sConfigMgr->GetOption<uint32>("ItemTalents.SoundCooldown", 30) * IN_MILLISECONDS;
    _soundOnKillChance = sConfigMgr->GetOption<uint32>("ItemTalents.SoundOnKillChance", 5);

    // Существо Фамильяра-фантома (прок SUMMON, пул H). Дефолт 191090 -
    // спиритовый фамильяр гачи (семья spirit, creature_template 191090-191099).
    _phantomCreature = sConfigMgr->GetOption<uint32>("ItemTalents.PhantomCreature", 191090);

    LOG_INFO("module", "mod-item-talents: enable={} maxRow={} masters={} range={:.1f} "
        "ignoreBots={} sounds={}", _enable, _maxImplementedRow, _masterEntries.size(),
        _masterRange, _ignoreBots, _itemSounds.size());
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

    // Колонка subclass добавляется миграцией фазы 2
    // (mod_item_talents_subclass.sql); без неё грузим по-старому (всё -1),
    // подкласс-специфичный ряд 5 недоступен.
    bool const hasSubclass = WorldColumnExists("item_talent_def", "subclass");
    if (!hasSubclass)
        LOG_WARN("module", "mod-item-talents: item_talent_def.subclass is missing - "
            "row 5 (subclass menus) inert. Apply "
            "pending_db_world/mod_item_talents_subclass.sql and restart.");

    QueryResult result = WorldDatabase.Query(hasSubclass
        ? "SELECT pool, `row`, choice, name_ru, desc_ru, effect, base, per_ilvl, subclass "
          "FROM item_talent_def"
        : "SELECT pool, `row`, choice, name_ru, desc_ru, effect, base, per_ilvl "
          "FROM item_talent_def");
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
        int16 const subclass = hasSubclass ? fields[8].Get<int16>() : int16(-1);
        if (pool.empty() || row < 1 || row > int8(MAX_ROWS) || choice < 1
            || choice > int8(MAX_MENU_CHOICES) || subclass < -1 || subclass > 20)
        {
            LOG_WARN("module", "mod-item-talents: skipped bad item_talent_def row "
                "(pool '{}', row {}, choice {}, subclass {}).", pool, row, choice, subclass);
            continue;
        }

        TalentDef def;
        def.nameRu = fields[3].Get<std::string>();
        def.descRu = fields[4].Get<std::string>();
        def.effect = fields[5].Get<std::string>();
        def.base = fields[6].Get<float>();
        def.perIlvl = fields[7].Get<float>();
        _defs[MakeKey(pool[0], uint8(row), uint8(choice), subclass)] = std::move(def);
        _menus[MakeKey(pool[0], uint8(row), 0, subclass)].push_back(uint8(choice));
        ++count;
    } while (result->NextRow());

    _defsLoaded = true;
    LOG_INFO("module", "mod-item-talents: loaded {} talent definitions in {} row menus.",
        count, _menus.size());

    // Именные наборы ряда 5 (item_talent_named). Таблица опциональна:
    // без неё модуль работает как раньше (ряд 5 закрыт).
    _named.clear();
    if (!WorldTableExists("item_talent_named"))
        LOG_INFO("module", "mod-item-talents: item_talent_named is missing - no named "
            "row-5 sets (apply pending_db_world/mod_item_talents_named.sql).");
    else if (QueryResult named = WorldDatabase.Query(
        "SELECT item_entry, choice, name_ru, desc_ru, effect, base, per_ilvl, proc_chance, "
        "icd_secs FROM item_talent_named"))
    {
        uint32 namedCount = 0;
        do
        {
            Field* fields = named->Fetch();
            uint32 const itemEntry = fields[0].Get<uint32>();
            uint8 const choice = fields[1].Get<uint8>();
            if (!itemEntry || choice < 1 || choice > MAX_SLOTS)
            {
                LOG_WARN("module", "mod-item-talents: skipped bad item_talent_named row "
                    "(entry {}, choice {}).", itemEntry, choice);
                continue;
            }

            ItemTalents::NamedDef& namedDef = _named[itemEntry][choice - 1];
            namedDef.def.nameRu = fields[2].Get<std::string>();
            namedDef.def.descRu = fields[3].Get<std::string>();
            namedDef.def.effect = fields[4].Get<std::string>();
            namedDef.def.base = fields[5].Get<float>();
            namedDef.def.perIlvl = fields[6].Get<float>();
            namedDef.procChance = fields[7].Get<uint8>();
            namedDef.icdSecs = fields[8].Get<uint32>();
            ++namedCount;
        } while (named->NextRow());

        LOG_INFO("module", "mod-item-talents: loaded {} named row-5 perks for {} items.",
            namedCount, _named.size());
    }

    // Ауры рядов 3 живут в spell_dbc (грузится при старте): без применённой
    // миграции mod_item_talents_aura_spells.sql проценты будут no-op.
    if (!sSpellMgr->GetSpellInfo(ItemTalents::SPELL_MOVE_SPEED_R1))
        LOG_WARN("module", "mod-item-talents: spell {} not found in spell_dbc - percent "
            "perks (rows 3) will be inert. Apply "
            "pending_db_world/mod_item_talents_aura_spells.sql and restart.",
            ItemTalents::SPELL_MOVE_SPEED_R1);

    // Ряд 5: параметры проков (item_talent_procs) + сброс кэша базовых эпиков
    _baseEpicCache.clear();
    _gaTableStatus = 0;
    LoadProcs();

    // Спеллы ряда 5 (триггеры + видимые) - тоже spell_dbc, нужен рестарт
    // после mod_item_talents_proc_spells.sql
    if (_procsLoaded && (!sSpellMgr->GetSpellInfo(ItemTalents::TRIGGER_SPELL_FIRST)
        || !sSpellMgr->GetSpellInfo(ItemTalents::VISIBLE_SPELL_FIRST)))
        LOG_WARN("module", "mod-item-talents: row-5 proc spells ({}/{}) not found in "
            "spell_dbc - awakening procs will be inert. Apply "
            "pending_db_world/mod_item_talents_proc_spells.sql and restart.",
            ItemTalents::TRIGGER_SPELL_FIRST, ItemTalents::VISIBLE_SPELL_FIRST);
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

TalentDef const* ItemTalentsMgr::GetDef(char pool, uint8 row, uint8 choice,
    int16 subclass) const
{
    // Точный подкласс -> фолбэк на -1 (любой)
    if (subclass >= 0)
    {
        auto exact = _defs.find(MakeKey(pool, row, choice, subclass));
        if (exact != _defs.end())
            return &exact->second;
    }

    auto itr = _defs.find(MakeKey(pool, row, choice, -1));
    return itr != _defs.end() ? &itr->second : nullptr;
}

ItemTalents::NamedDef const* ItemTalentsMgr::GetNamedDef(uint32 itemEntry, uint8 choice) const
{
    if (choice < 1 || choice > MAX_SLOTS)
        return nullptr;

    auto itr = _named.find(itemEntry);
    if (itr == _named.end())
        return nullptr;

    ItemTalents::NamedDef const& namedDef = itr->second[choice - 1];
    return namedDef.def.effect.empty() ? nullptr : &namedDef;
}

bool ItemTalentsMgr::IsBaseEpic(ItemTemplate const* proto) const
{
    if (!proto)
        return false;

    // Ручные диапазоны из конфига (стиль ahbot) сильнее любых проверок:
    // deny запрещает ряд 5, allow принудительно разрешает.
    if (EntryInRanges(_row5Deny, proto->ItemId))
        return false;
    if (EntryInRanges(_row5Allow, proto->ItemId))
        return true;

    if (proto->Quality != ITEM_QUALITY_EPIC)
        return false;

    auto cached = _baseEpicCache.find(proto->ItemId);
    if (cached != _baseEpicCache.end())
        return cached->second;

    // Без GA-таблицы (модуль mod-gear-ascension отсутствует) цепочек нет:
    // любой эпик - базовый. Известная грабля: SELECT из несуществующей
    // таблицы абортит worldserver - гейт через information_schema.
    if (!_gaTableStatus)
    {
        _gaTableStatus = WorldTableExists("item_upgrade_chain") ? 1 : -1;
        if (_gaTableStatus < 0)
            LOG_INFO("module", "mod-item-talents: item_upgrade_chain is missing "
                "(mod-gear-ascension not installed?) - all epics count as base epics.");
    }

    bool baseEpic = true;
    if (_gaTableStatus > 0)
    {
        // Цепочка ключуется ИСХОДНЫМ предметом (entry -> next_entry):
        // вершина (эпик-копия) собственной строки НЕ имеет, поэтому ищем
        // предмет в колонке next_entry - нашёлся значит это GA-копия,
        // а копия базовым эпиком не бывает (эпик-базы GA не апгрейдит).
        QueryResult result = WorldDatabase.Query(
            "SELECT 1 FROM item_upgrade_chain WHERE next_entry = {} LIMIT 1",
            proto->ItemId);
        baseEpic = !result;
    }

    _baseEpicCache[proto->ItemId] = baseEpic;
    return baseEpic;
}

bool ItemTalentsMgr::EntryInRanges(std::vector<std::pair<uint32, uint32>> const& ranges,
    uint32 entry)
{
    for (auto const& [lo, hi] : ranges)
        if (entry >= lo && entry <= hi)
            return true;

    return false;
}

uint8 ItemTalentsMgr::RowsOpenForItem(ItemTemplate const* proto) const
{
    if (!proto)
        return 0;

    uint8 const rows = RowsOpenForQuality(proto->Quality);
    // Фаза 2 (решение 2026-07-06): ряд 5 открывается УЖЕ НА ЭПИКЕ у именных
    // наборов и у базовых эпиков; GA-копии с корнем ниже эпика - потолок 4.
    if (rows == 4 && (HasNamedSet(proto->ItemId) || IsBaseEpic(proto)))
        return MAX_ROWS;

    return rows;
}

bool ItemTalentsMgr::IsRowSelectable(ItemTemplate const* proto, uint8 row) const
{
    if (row <= _maxImplementedRow)
        return true;

    if (row != MAX_ROWS || !proto)
        return false;

    // Именной ряд 5 работает и без item_talent_procs (пассивы Джордана);
    // generic-проки требуют загруженных параметров.
    return HasNamedSet(proto->ItemId) || (_procsLoaded && IsBaseEpic(proto));
}

TalentDef const* ItemTalentsMgr::GetDefForItem(ItemTemplate const* proto, uint8 row,
    uint8 choice) const
{
    if (!proto)
        return nullptr;

    if (row == MAX_ROWS)
        if (ItemTalents::NamedDef const* namedDef = GetNamedDef(proto->ItemId, choice))
            return &namedDef->def;

    std::optional<char> pool = GetPool(proto->Class, proto->SubClass);
    return pool ? GetDef(*pool, row, choice, int16(proto->SubClass)) : nullptr;
}

std::vector<uint8> const* ItemTalentsMgr::GetMenu(char pool, uint8 row, int16 subclass) const
{
    if (subclass >= 0)
    {
        auto exact = _menus.find(MakeKey(pool, row, 0, subclass));
        if (exact != _menus.end())
            return &exact->second;
    }

    auto itr = _menus.find(MakeKey(pool, row, 0, -1));
    return itr != _menus.end() ? &itr->second : nullptr;
}

int32 ItemTalentsMgr::CalcValue(TalentDef const& def, uint32 itemLevel, uint8 quality) const
{
    // Проки ряда 5: base хранит spell id триггера, значение считается ТОЛЬКО
    // от per_ilvl; роллы качества на проки не распространяются (PERKS
    // "Роллы и качество"). coef 0 = у эффекта фиксированные basepoints в dbc.
    if (def.effect == "PROC")
        return def.perIlvl > 0.0f
            ? std::max(1, int32(std::ceil(def.perIlvl * float(itemLevel)))) : 0;

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
    if (!player)
        return;

    ObjectGuid::LowType const ownerGuid = player->GetGUID().GetCounter();
    _states.erase(ownerGuid);
    // Агрегат перков умирает вместе с сессией (статы игрока ядро всё равно
    // пересчитает на следующем логине с нуля).
    _perks.erase(ownerGuid);
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

    ItemTemplate const* proto = item->GetTemplate();
    if (!proto)
        return;

    std::optional<char> pool = GetPool(proto->Class, proto->SubClass);
    if (!pool)
        return;

    std::string values;
    for (uint8 row = 1; row <= MAX_ROWS; ++row)
    {
        // Роллим по-рядно: ряд с любым роллом пропускаем. Это и защита от
        // повторного ролла, и дороллинг ряда 5 именным предметам, чьи
        // ряды 1-4 были роллены до появления именного набора.
        bool alreadyRolled = false;
        for (RollSlot const& roll : state->rolls[row - 1])
            if (roll.choice)
            {
                alreadyRolled = true;
                break;
            }
        if (alreadyRolled)
            continue;

        // Именной ряд 5: все 3 перка фиксированно (choice = слот),
        // БЕЗ качества (quality всегда 0 - именные перки одного качества).
        if (row == MAX_ROWS && HasNamedSet(proto->ItemId))
        {
            for (uint8 slot = 1; slot <= MAX_SLOTS; ++slot)
            {
                if (!GetNamedDef(proto->ItemId, slot))
                    continue;

                RollSlot& roll = state->rolls[row - 1][slot - 1];
                roll.choice = slot;
                roll.quality = 0;

                if (!values.empty())
                    values += ',';
                values += Acore::StringFormat("({},{},{},{},{})",
                    item->GetGUID().GetCounter(), row, slot, roll.choice, roll.quality);
            }
            continue;
        }

        std::vector<uint8> const* menu = GetMenu(*pool, row, int16(proto->SubClass));
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
                // Ряд 5 - проки БЕЗ качества (PERKS "Роллы и качество":
                // роллы качеств на проки не распространяются)
                roll.quality = row == MAX_ROWS ? 0 : RollQuality();
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

void ItemTalentsMgr::TransferItem(ObjectGuid::LowType oldItemGuid,
    ObjectGuid::LowType newItemGuid, Player* player)
{
    if (!oldItemGuid || !newItemGuid || oldItemGuid == newItemGuid)
        return;

    // Строки переносим и при выключенном модуле (данные не сиротим), но
    // только если таблицы существуют - одноразовый кэшируемый чек.
    if (!IsEnabled())
    {
        static int8 tablesExist = 0; // 0 = не проверяли, 1 = есть, -1 = нет
        if (!tablesExist)
            tablesExist = (CharTableExists("item_talents")
                && CharTableExists("item_talent_rolls")) ? 1 : -1;
        if (tablesExist < 0)
            return;
    }

    // БД синхронно: сразу после GA-свопа модуль может сделать точечный SELECT
    // нового GUID (EnsureState) - перенос обязан быть уже виден.
    CharacterDatabase.DirectExecute(
        "UPDATE item_talents SET item_guid = {} WHERE item_guid = {}", newItemGuid, oldItemGuid);
    CharacterDatabase.DirectExecute(
        "UPDATE item_talent_rolls SET item_guid = {} WHERE item_guid = {}",
        newItemGuid, oldItemGuid);

    // Кэш: перекладываем состояние (включая несброшенные dirtyKills - их
    // допишет FlushKills уже под новым GUID) на новый ключ у владельца.
    if (!player)
        return;

    auto ownerItr = _states.find(player->GetGUID().GetCounter());
    if (ownerItr == _states.end())
        return;

    auto node = ownerItr->second.extract(oldItemGuid);
    if (node.empty())
        return;

    node.key() = newItemGuid;
    ownerItr->second.insert(std::move(node));
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

void ItemTalentsMgr::SetKills(Player* player, Item* item, uint32 kills)
{
    ItemState& state = EnsureState(player, item);
    state.kills = kills;
    state.dirtyKills = 0; // старая дельта поглощается выставленным значением

    CharacterDatabase.DirectExecute(
        "INSERT INTO item_talents (item_guid, owner_guid, kills) VALUES ({}, {}, {}) "
        "ON DUPLICATE KEY UPDATE kills = {}, owner_guid = VALUES(owner_guid)",
        item->GetGUID().GetCounter(), player->GetGUID().GetCounter(), kills, kills);
}

void ItemTalentsMgr::RerollItem(Player* player, Item* item)
{
    ItemState& state = EnsureState(player, item);
    ItemTemplate const* proto = item->GetTemplate();

    // Снять применённые статы выбранных рядов. Клампим по открытым предмету
    // рядам - зеркально ApplyAllTalents (ряды выше не применялись).
    uint8 const rowsOpen = RowsOpenForItem(proto);
    if (item->IsEquipped() && !item->IsBroken())
        for (uint8 row = 1; row <= std::min<uint8>(rowsOpen, MAX_ROWS); ++row)
            if (uint8 slot = state.rows[row - 1])
                ApplyTalent(player, item, row, slot, false);

    state.rows = { };
    state.rolls = { };

    ObjectGuid::LowType const itemGuid = item->GetGUID().GetCounter();
    CharacterDatabase.DirectExecute(
        "UPDATE item_talents SET row1 = 0, row2 = 0, row3 = 0, row4 = 0, row5 = 0 "
        "WHERE item_guid = {}", itemGuid);
    CharacterDatabase.DirectExecute(
        "DELETE FROM item_talent_rolls WHERE item_guid = {}", itemGuid);

    EnsureRolled(player, item); // свежий ролл + batched INSERT
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

    // Именной ряд 5 - из item_talent_named (плюс прок-поля), иначе сид пула
    if (row == MAX_ROWS)
        if (ItemTalents::NamedDef const* namedDef = GetNamedDef(proto->ItemId, roll.choice))
        {
            // Именной прок общего движка (Гнев Джордана): base = триггер-спелл
            if (namedDef->def.effect == "PROC")
                ApplyProc(player, item, uint32(namedDef->def.base),
                    CalcValue(namedDef->def, proto->ItemLevel, roll.quality), apply);
            else
                ApplyEffect(player, item, namedDef->def.effect,
                    CalcValue(namedDef->def, proto->ItemLevel, roll.quality), apply,
                    namedDef->procChance, namedDef->icdSecs);
            return;
        }

    std::optional<char> pool = GetPool(proto->Class, proto->SubClass);
    if (!pool)
        return;

    TalentDef const* def = GetDef(*pool, row, roll.choice, int16(proto->SubClass));
    if (!def)
        return;

    // Проки ряда 5 (effect='PROC'): регистрация в движке, не статы
    if (def->effect == "PROC")
    {
        ApplyProc(player, item, uint32(def->base),
            CalcValue(*def, proto->ItemLevel, roll.quality), apply);
        return;
    }

    ApplyEffect(player, item, def->effect, CalcValue(*def, proto->ItemLevel, roll.quality),
        apply);
}

void ItemTalentsMgr::ApplyAllTalents(Player* player, Item* item, bool apply)
{
    if (!IsEnabled() || !player || !item)
        return;

    ItemTemplate const* proto = item->GetTemplate();
    if (!IsEligibleItem(proto))
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

    // Ряды выше открытых предмету не применяем: после GA-даунгрейда (эпик ->
    // синий при провале) выбор ряда 4 сохраняется в БД, но эффект спит, пока
    // качество не вернут. Симметрично для apply/unapply - качество у GUID
    // неизменно (GA-своп создаёт новый GUID).
    uint8 const rowsOpen = std::min<uint8>(RowsOpenForItem(proto), MAX_ROWS);
    for (uint8 row = 1; row <= rowsOpen; ++row)
        if (uint8 slot = state->rows[row - 1])
            ApplyTalent(player, item, row, slot, apply);
}

// Зеркалит Player::_ApplyItemBonuses для соответствующих ITEM_MOD_*.
// Эффекты рядов 3-4 агрегируются в PlayerPerks (см. шапку файла);
// procChance/icdSecs приходят только от именных прок-перков ряда 5.
void ItemTalentsMgr::ApplyEffect(Player* player, Item* item, std::string const& effect,
    int32 value, bool apply, uint8 procChance, uint32 icdSecs)
{
    float const fval = float(value);
    int32 const delta = apply ? value : -value;

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
    else if (effect == "DURA_SAVE")
    {
        // Шанс пропуска события износа хранится по GUID предмета - хук
        // OnPlayerDurabilityPointsLoss бьёт только по самому предмету.
        PlayerPerks& perks = EnsurePerks(player);
        if (apply)
            perks.duraSavePct[item->GetGUID().GetCounter()] =
                uint8(std::clamp<int32>(value, 0, 100));
        else
            perks.duraSavePct.erase(item->GetGUID().GetCounter());
    }
    else if (effect == "MOVE_SPEED_PCT")
    {
        PlayerPerks& perks = EnsurePerks(player);
        perks.moveSpeedPct = std::max(0, perks.moveSpeedPct + delta);
        UpdateRankAura(player, perks.moveSpeedPct, _capMoveSpeedPct,
            ItemTalents::SPELL_MOVE_SPEED_R1);
    }
    else if (effect == "HEAL_TAKEN_PCT")
    {
        PlayerPerks& perks = EnsurePerks(player);
        perks.healTakenPct = std::max(0, perks.healTakenPct + delta);
        UpdateRankAura(player, perks.healTakenPct, _capHealTakenPct,
            ItemTalents::SPELL_HEAL_TAKEN_R1);
    }
    else if (effect == "PHYS_TAKEN_PCT")
    {
        PlayerPerks& perks = EnsurePerks(player);
        perks.physTakenPct = std::max(0, perks.physTakenPct + delta);
        UpdateRankAura(player, perks.physTakenPct, _capPhysTakenPct,
            ItemTalents::SPELL_PHYS_TAKEN_R1);
    }
    else if (effect == "NEMESIS_DMG_PCT")
    {
        PlayerPerks& perks = EnsurePerks(player);
        perks.nemesisDmgPct = std::max(0, perks.nemesisDmgPct + delta);
    }
    else if (effect == "FAMILIAR_ALL_STATS")
    {
        PlayerPerks& perks = EnsurePerks(player);
        perks.familiarStats = std::max(0, perks.familiarStats + delta);
        RecalcFamiliarStats(player);
    }
    else if (effect == "GOLD_XP_PCT")
    {
        // value = процент золота (скейлится качеством); опыт - фикс. +1% за
        // предмет, качеством НЕ скейлится (решение 2026-07-06).
        PlayerPerks& perks = EnsurePerks(player);
        perks.goldPct = std::max(0, perks.goldPct + delta);
        perks.xpPct = std::max(0, perks.xpPct + (apply ? 1 : -1));
    }
    else if (effect == "MANA_FLAT") // как ITEM_MOD_MANA (_ApplyItemBonuses)
        player->HandleStatFlatModifier(UNIT_MOD_MANA, BASE_VALUE, fval, apply);
    else if (effect == "JORDAN_COIN" || effect == "JORDAN_LIGHTNING")
    {
        // Именные проки: регистрируем/снимаем в списке активных проков
        PlayerPerks& perks = EnsurePerks(player);
        std::vector<ItemTalents::ActiveProc>& procs =
            effect == "JORDAN_COIN" ? perks.coinProcs : perks.lightningProcs;
        ObjectGuid::LowType const itemGuid = item->GetGUID().GetCounter();
        if (apply)
        {
            ItemTalents::ActiveProc proc;
            proc.itemGuid = itemGuid;
            proc.itemEntry = item->GetEntry();
            proc.value = value;
            proc.procChance = procChance;
            proc.icdMs = icdSecs * IN_MILLISECONDS;
            procs.push_back(proc);
        }
        else
            std::erase_if(procs, [itemGuid](ItemTalents::ActiveProc const& proc)
            {
                return proc.itemGuid == itemGuid;
            });
    }
    else
        LOG_WARN("module", "mod-item-talents: unknown effect '{}' in item_talent_def.", effect);
}

// ---------------------------------------------------------------------------
// Именные проки ряда 5
// ---------------------------------------------------------------------------

bool ItemTalentsMgr::RollProc(ItemTalents::ActiveProc& proc)
{
    if (!proc.procChance)
        return false;

    uint32 const now = getMSTime();
    if (proc.lastMs && getMSTimeDiff(proc.lastMs, now) < proc.icdMs)
        return false;

    if (!roll_chance_i(int32(proc.procChance)))
        return false;

    proc.lastMs = now;
    return true;
}

// Убийство: проки ряда 5 (KILL) + именной JORDAN_COIN. Жертва -
// OnPlayerCreatureKill (существо), PvE-гейт выполнен вызывающим.
void ItemTalentsMgr::OnKillProcs(Player* player)
{
    if (!IsEnabled() || !player)
        return;

    // Ряд 5: KILL-проки (Гальваника, Пир победителя, Скользящая тень,
    // Хладнокровие) - эффекты на себя, цель не нужна
    HandleProcEvent(player, ItemTalents::ProcTrigger::KILL, nullptr);

    PlayerPerks* perks = GetPerks(player->GetGUID().GetCounter());
    if (!perks || perks->coinProcs.empty())
        return;

    for (ItemTalents::ActiveProc& proc : perks->coinProcs)
    {
        if (!RollProc(proc))
            continue;

        player->ModifyMoney(proc.value);
        ChatHandler(player->GetSession()).PSendSysMessage(
            "|cffffd700Предмет чеканит вам {} меди.|r", proc.value);
        if (Item* item = player->GetItemByGuid(
            ObjectGuid::Create<HighGuid::Item>(proc.itemGuid)))
            PlayItemSound(player, item);
    }
}

// Успешный боевой каст по врагу: проки ряда 5 (CAST_HARMFUL - Всплеск чар,
// Эхо маны, Фамильяр-фантом, Гнев Джордана) + легаси JORDAN_LIGHTNING
// (NPC-спелл 9532 с bp-override; остаётся до применения миграции фазы 2).
void ItemTalentsMgr::OnCombatSpellCast(Player* player, Unit* target, uint32 spellId)
{
    if (!IsEnabled() || !player || !target)
        return;

    // Не реагируем на собственные прок-спеллы (рекурсия Spell::cast)
    if (spellId == ItemTalents::SPELL_GENERIC_LIGHTNING_BOLT
        || (spellId >= ItemTalents::TRIGGER_SPELL_FIRST
            && spellId <= ItemTalents::VISIBLE_SPELL_LAST))
        return;

    // Только PvE (решение 2026-07-06): цель - существо не под игроком
    if (!IsPveUnit(target))
        return;

    HandleProcEvent(player, ItemTalents::ProcTrigger::CAST_HARMFUL, target);

    PlayerPerks* perks = GetPerks(player->GetGUID().GetCounter());
    if (!perks || perks->lightningProcs.empty())
        return;

    for (ItemTalents::ActiveProc& proc : perks->lightningProcs)
    {
        if (!RollProc(proc))
            continue;

        int32 bp0 = proc.value;
        player->CastCustomSpell(target, ItemTalents::SPELL_GENERIC_LIGHTNING_BOLT,
            &bp0, nullptr, nullptr, true);
        if (Item* item = player->GetItemByGuid(
            ObjectGuid::Create<HighGuid::Item>(proc.itemGuid)))
            PlayItemSound(player, item);
    }
}

// ---------------------------------------------------------------------------
// Агрегат перков рядов 3-4
// ---------------------------------------------------------------------------

PlayerPerks* ItemTalentsMgr::GetPerks(ObjectGuid::LowType ownerGuid)
{
    auto itr = _perks.find(ownerGuid);
    return itr != _perks.end() ? &itr->second : nullptr;
}

PlayerPerks const* ItemTalentsMgr::GetPerks(ObjectGuid::LowType ownerGuid) const
{
    auto itr = _perks.find(ownerGuid);
    return itr != _perks.end() ? &itr->second : nullptr;
}

PlayerPerks& ItemTalentsMgr::EnsurePerks(Player* player)
{
    return _perks[player->GetGUID().GetCounter()];
}

void ItemTalentsMgr::UpdateRankAura(Player* player, int32 totalPct, int32 capPct,
    uint32 firstSpellId) const
{
    // Ранг = min(сумма по надетым, кап, число рангов); 0 - снять всё.
    int32 const rank = std::min({ totalPct, capPct, int32(AURA_RANKS) });

    for (uint8 i = 0; i < AURA_RANKS; ++i)
    {
        uint32 const spellId = firstSpellId + i;
        if (rank == int32(i) + 1)
        {
            if (!player->HasAura(spellId))
                player->AddAura(spellId, player); // no-op с warn ядра, если спелла нет в spell_dbc
        }
        else
            player->RemoveAurasDueToSpell(spellId);
    }
}

bool ItemTalentsMgr::IsFamiliarOwnerAura(uint32 spellId) const
{
    for (auto const& [lo, hi] : _familiarAuraRanges)
        if (spellId >= lo && spellId <= hi)
            return true;

    return false;
}

bool ItemTalentsMgr::HasFamiliarOwnerAura(Player* player) const
{
    for (auto const& pair : player->GetAppliedAuras())
        if (IsFamiliarOwnerAura(pair.first))
            return true;

    return false;
}

void ItemTalentsMgr::OnFamiliarAuraChanged(Player* player)
{
    if (!IsEnabled() || !player)
        return;

    // Пересчёт нужен только тем, у кого выбран перк "Узы фамильяра"
    PlayerPerks* perks = GetPerks(player->GetGUID().GetCounter());
    if (!perks || (!perks->familiarStats && !perks->familiarApplied))
        return;

    RecalcFamiliarStats(player);
}

void ItemTalentsMgr::RecalcFamiliarStats(Player* player)
{
    PlayerPerks* perks = GetPerks(player->GetGUID().GetCounter());
    if (!perks)
        return;

    // Бонус активен, только пока фамильяр призван (владельческая аура на игроке)
    int32 const target = HasFamiliarOwnerAura(player) ? perks->familiarStats : 0;
    int32 const delta = target - perks->familiarApplied;
    if (!delta)
        return;

    bool const apply = delta > 0;
    float const amount = float(std::abs(delta));

    static constexpr std::pair<UnitMods, Stats> statMods[] =
    {
        { UNIT_MOD_STAT_STRENGTH,  STAT_STRENGTH  },
        { UNIT_MOD_STAT_AGILITY,   STAT_AGILITY   },
        { UNIT_MOD_STAT_STAMINA,   STAT_STAMINA   },
        { UNIT_MOD_STAT_INTELLECT, STAT_INTELLECT },
        { UNIT_MOD_STAT_SPIRIT,    STAT_SPIRIT    },
    };
    for (auto const& [unitMod, stat] : statMods)
    {
        player->HandleStatFlatModifier(unitMod, BASE_VALUE, amount, apply);
        player->UpdateStatBuffMod(stat);
    }

    perks->familiarApplied = target;
}

// ---------------------------------------------------------------------------
// Эффекты рядов 3-4: хуки
// ---------------------------------------------------------------------------

void ItemTalentsMgr::HandleDurabilityLoss(Player* player, Item* item, int32& points)
{
    if (!IsEnabled() || !player || !item || points <= 0)
        return;

    PlayerPerks const* perks = GetPerks(player->GetGUID().GetCounter());
    if (!perks)
        return;

    auto itr = perks->duraSavePct.find(item->GetGUID().GetCounter());
    if (itr == perks->duraSavePct.end())
        return;

    // Неснашиваемость: N% событий износа этого предмета пропускается
    if (roll_chance_i(int32(itr->second)))
        points = 0;
}

uint32 ItemTalentsMgr::GetGoldBonusPct(Player* player) const
{
    if (!IsEnabled() || !player)
        return 0;

    PlayerPerks const* perks = GetPerks(player->GetGUID().GetCounter());
    if (!perks || perks->goldPct <= 0)
        return 0;

    return uint32(std::min(perks->goldPct, _capGoldPct));
}

uint32 ItemTalentsMgr::GetXpBonusPct(Player* player) const
{
    if (!IsEnabled() || !player)
        return 0;

    PlayerPerks const* perks = GetPerks(player->GetGUID().GetCounter());
    if (!perks || perks->xpPct <= 0)
        return 0;

    return uint32(std::min(perks->xpPct, _capXpPct));
}

uint32 ItemTalentsMgr::GetNemesisBonusPct(Player* player) const
{
    if (!IsEnabled() || !player)
        return 0;

    PlayerPerks const* perks = GetPerks(player->GetGUID().GetCounter());
    if (!perks || perks->nemesisDmgPct <= 0)
        return 0;

    return uint32(std::min(perks->nemesisDmgPct, _capNemesisDmgPct));
}

bool ItemTalentsMgr::IsNemesisTarget(Creature* creature) const
{
    if (!creature)
        return false;

    // TODO(nemesis-dev): надёжного внешнего признака немезиды нет - ActiveNemeses
    // живёт в анонимном namespace mod-nemesis-system, визуальная аура
    // (NemesisSystem.VisualAuraSpell) на сервере выключена (= 0). Кэш ниже
    // покрывает персистентных мировых немезид (ключ character_nemesis.guid =
    // spawnId), но НЕ видит: (а) временных данжевых немезид
    // (ActiveTemporaryNemeses, в БД не пишутся), (б) редких мобов после
    // ротации пула (активный спавн может иметь другой spawnId, чем строка
    // в БД). Чистое решение: публичный заголовок NemesisAPI.h в
    // mod-nemesis-system (по образцу CitySiegeAPI.h) с
    // bool NemesisAPI::IsNemesis(Creature*) поверх TryGetNemesisState -
    // тогда здесь ветка #if __has_include("NemesisAPI.h").
    ObjectGuid::LowType const spawnId = creature->GetSpawnId();
    return spawnId && _nemesisSpawnIds.contains(spawnId);
}

void ItemTalentsMgr::UpdateNemesisCache(uint32 diff)
{
    if (!IsEnabled())
        return;

    _nemesisRefreshTimer += diff;
    if (_nemesisRefreshTimer < _nemesisRefreshMs)
        return;

    _nemesisRefreshTimer = 0;

    // Дёшево: обновляем, только если хоть у кого-то онлайн выбран перк
    bool wanted = false;
    for (auto const& [ownerGuid, perks] : _perks)
        if (perks.nemesisDmgPct > 0)
        {
            wanted = true;
            break;
        }

    if (!wanted)
    {
        _nemesisSpawnIds.clear();
        return;
    }

    if (!_nemesisTableStatus)
    {
        _nemesisTableStatus = CharTableExists("character_nemesis") ? 1 : -1;
        if (_nemesisTableStatus < 0)
            LOG_WARN("module", "mod-item-talents: character_nemesis table is missing "
                "(mod-nemesis-system not installed?) - NEMESIS_DMG_PCT perk is inert.");
    }

    if (_nemesisTableStatus < 0)
        return;

    _nemesisSpawnIds.clear();
    if (QueryResult result = CharacterDatabase.Query("SELECT guid FROM character_nemesis"))
        do
        {
            _nemesisSpawnIds.insert(ObjectGuid::LowType(result->Fetch()[0].Get<uint64>()));
        } while (result->NextRow());
}

// ---------------------------------------------------------------------------
// Звук пробуждённого предмета
// ---------------------------------------------------------------------------

bool ItemTalentsMgr::IsFullyAwakened(ItemState const& state, ItemTemplate const* proto) const
{
    if (!proto)
        return false;

    uint8 const rowsOpen = std::min<uint8>(RowsOpenForItem(proto), MAX_ROWS);
    if (!rowsOpen)
        return false;

    // v1-послабление (решение 2026-07-06): "полностью пробуждён" = выбраны
    // все ВЫБИРАЕМЫЕ ряды, открытые предмету, и не меньше MaxImplementedRow
    // рядов (невыбираемый в v1 ряд 5 обычных предметов не учитывается;
    // именной ряд 5 - учитывается). Финальное правило фазы 2 - все 5 рядов.
    if (rowsOpen < _maxImplementedRow)
        return false;

    for (uint8 row = 1; row <= rowsOpen; ++row)
    {
        if (!IsRowSelectable(proto, row))
            continue;

        if (!state.rows[row - 1])
            return false;
    }

    return true;
}

uint32 ItemTalentsMgr::GetItemSoundId(uint32 itemEntry) const
{
    auto itr = _itemSounds.find(itemEntry);
    return itr != _itemSounds.end() ? itr->second : 0;
}

bool ItemTalentsMgr::PlayItemSound(Player* player, Item* item)
{
    if (!IsEnabled() || !player || !item)
        return false;

    // На логине (_ApplyItemMods до входа в мир) звук не играем
    if (!player->IsInWorld())
        return false;

    uint32 const soundId = GetItemSoundId(item->GetEntry());
    if (!soundId)
        return false;

    // Именной звук - только у ОРУЖИЯ в основной руке (решение 2026-07-06;
    // двуручное тоже занимает этот слот)
    if (!item->IsEquipped() || item->GetSlot() != EQUIPMENT_SLOT_MAINHAND)
        return false;

    ItemState* state = GetState(player->GetGUID().GetCounter(), item->GetGUID().GetCounter());
    if (!state || !IsFullyAwakened(*state, item->GetTemplate()))
        return false;

    // Троттлинг на игрока (проки фазы 2 будут дёргать этот же метод)
    PlayerPerks& perks = EnsurePerks(player);
    uint32 const now = getMSTime();
    if (perks.lastSoundMs && getMSTimeDiff(perks.lastSoundMs, now) < _soundCooldownMs)
        return false;

    perks.lastSoundMs = now;
    player->PlayDirectSound(soundId, player); // только владельцу
    return true;
}

void ItemTalentsMgr::OnKillSoundChance(Player* player)
{
    if (!IsEnabled() || !player || !_soundOnKillChance)
        return;

    if (!roll_chance_i(int32(_soundOnKillChance)))
        return;

    // Звук настроен только для оружия основной руки - смотрим один слот
    if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND))
        PlayItemSound(player, item);
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

char const* ItemTalentsMgr::ValidateUsableItem(Player* player, Item* item) const
{
    if (!player || !item)
        return "NO_ITEM";

    if (!item->IsEquipped())
        return "NOT_EQUIPPED";

    if (!IsEligibleItem(item->GetTemplate()))
        return "NO_POOL";

    // На сломанном предмете статы ядром сняты - менять таланты нельзя,
    // иначе разъедутся применённые модификаторы.
    if (item->IsBroken())
        return "BROKEN";

    return nullptr;
}

char const* ItemTalentsMgr::TryChoose(Player* player, Item* item, uint8 row, uint8 slot,
    bool requireNearMaster)
{
    if (!IsEnabled())
        return "DISABLED";

    if (row < 1 || row > MAX_ROWS || slot < 1 || slot > MAX_SLOTS)
        return "BAD_ARGS";

    if (char const* error = ValidateUsableItem(player, item))
        return error;

    ItemTemplate const* proto = item->GetTemplate();

    if (row > RowsOpenForItem(proto))
        return "ROW_LOCKED"; // ряд не открыт качеством (именной ряд 5 - на эпике)

    if (!IsRowSelectable(proto, row))
        return "ROW_SOON"; // ряды выше показываются, но выбор запрещён

    // EnsureState лениво роллит слоты (EnsureRolled внутри)
    ItemState& state = EnsureState(player, item);

    // Слот должен быть роллен, а его choice - существовать в сиде/именном наборе
    RollSlot const& roll = state.rolls[row - 1][slot - 1];
    if (!roll.choice || !GetDefForItem(proto, row, roll.choice))
        return "BAD_CHOICE";

    if (state.rows[row - 1])
        return "ALREADY_CHOSEN";

    if (!FreePoints(state))
        return "NO_POINTS";

    // Госсип у мастера пропускает проверку радиуса - игрок и так у НПЦ
    if (requireNearMaster && !IsNearMaster(player))
        return "NO_MASTER";

    SaveChoice(player, item, row, slot);
    ApplyTalent(player, item, row, slot, true);

    // Первый вложенный талант привязывает предмет (DESIGN §2)
    if (!item->IsSoulBound())
    {
        item->SetBinding(true);
        item->SetState(ITEM_CHANGED, player);
    }

    return nullptr;
}

char const* ItemTalentsMgr::TryReset(Player* player, Item* item, uint8 row,
    bool requireNearMaster)
{
    if (!IsEnabled())
        return "DISABLED";

    if (row < 1 || row > MAX_ROWS)
        return "BAD_ARGS";

    if (char const* error = ValidateUsableItem(player, item))
        return error;

    ItemState& state = EnsureState(player, item);
    uint8 const oldSlot = state.rows[row - 1]; // rowN = выбранный слот 1..3
    if (!oldSlot)
        return "NOT_CHOSEN";

    if (requireNearMaster && !IsNearMaster(player))
        return "NO_MASTER";

    // Ряды выше открытых предмету не применялись (кламп в ApplyAllTalents
    // после GA-даунгрейда) - снимать их эффект нельзя, иначе статы уедут.
    if (row <= RowsOpenForItem(item->GetTemplate()))
        ApplyTalent(player, item, row, oldSlot, false);

    ResetChoice(player, item, row);
    return nullptr;
}

bool ItemTalentsMgr::IsNearMaster(Player* player) const
{
    for (uint32 entry : _masterEntries)
        if (player->FindNearestCreature(entry, _masterRange))
            return true;

    return false;
}

bool ItemTalentsMgr::IsMasterEntry(uint32 entry) const
{
    return std::find(_masterEntries.begin(), _masterEntries.end(), entry)
        != _masterEntries.end();
}
