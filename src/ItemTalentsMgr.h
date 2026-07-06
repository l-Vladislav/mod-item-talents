/*
 * mod-item-talents: менеджер (синглтон).
 *
 * Держит кэш определений талантов (acore_world.item_talent_def), кэш
 * состояния предметов (acore_characters.item_talents + item_talent_rolls)
 * и применяет статы напрямую (без spell_dbc), зеркаля вызовы ядра из
 * Player::_ApplyItemBonuses.
 *
 * Меню ряда (pool, row) держит до MAX_MENU_CHOICES вариантов; предмет при
 * первом обращении лениво роллит 3 случайных варианта с качеством
 * (EnsureRolled), UI остаётся "выбор 1 из 3 слотов".
 *
 * Дизайн: .claude/item-talents/DESIGN.md (особенно §2, §4, §6) и PERKS.md.
 */

#ifndef MOD_ITEM_TALENTS_MGR_H
#define MOD_ITEM_TALENTS_MGR_H

#include "Define.h"
#include "ObjectGuid.h"
#include <array>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class Item;
class Player;
struct ItemTemplate;

namespace ItemTalents
{
    constexpr uint8 MAX_ROWS = 5;
    constexpr uint8 MAX_SLOTS = 3;         // роллы-слоты ряда (UI: выбор 1 из 3)
    constexpr uint8 MAX_MENU_CHOICES = 15; // потолок choice в сиде item_talent_def
    constexpr uint8 NUM_QUALITIES = 3;     // Обычный / Отличный / Совершенный

    // Строка item_talent_def (acore_world)
    struct TalentDef
    {
        std::string nameRu;
        std::string descRu;
        std::string effect;   // STAT_STA, RATING_CRIT, MP5, ...
        float base = 0.0f;
        float perIlvl = 0.0f; // > 0: флэт = max(1, ceil(base + perIlvl * ilvl)); 0: процент в base
    };

    // Ролл одного слота ряда (строка item_talent_rolls, acore_characters)
    struct RollSlot
    {
        uint8 choice = 0;  // choice из меню (pool, row); 0 = слот не роллен
        uint8 quality = 0; // 0..2 - индекс в ItemTalents.QualityMults
    };

    // Состояние одного предмета (кэш item_talents + item_talent_rolls)
    struct ItemState
    {
        std::array<uint8, MAX_ROWS> rows = { }; // 0 = не выбран, 1..3 = выбранный СЛОТ
        std::array<std::array<RollSlot, MAX_SLOTS>, MAX_ROWS> rolls = { };
        uint32 kills = 0;                       // текущее значение (БД + несброшенная дельта)
        uint32 dirtyKills = 0;                  // дельта убийств, ещё не записанная в БД
    };
}

class ItemTalentsMgr
{
public:
    static ItemTalentsMgr* instance();

    void LoadConfig();      // конфиг (OnStartup; повторно - на перезагрузке конфига)
    void LoadDefinitions(); // ТОЛЬКО из WorldScript::OnStartup (БД уже доступна)

    [[nodiscard]] bool IsEnabled() const { return _enable && _defsLoaded; }
    [[nodiscard]] uint8 GetMaxImplementedRow() const { return _maxImplementedRow; }

    // Плейербот при ItemTalents.IgnoreBots = 1: вне системы (не копит опыт,
    // не грузит кэш). Определение бота - как в mod-ollama-chat (PlayerbotsMgr).
    [[nodiscard]] bool ShouldIgnorePlayer(Player* player) const;

    // ---- статика по предмету ----
    // (class, subclass) -> пул A..H; nullopt = предмет вне системы (DESIGN §4)
    static std::optional<char> GetPool(uint32 itemClass, uint32 itemSubClass);
    // качество 1..5+ -> 1..5 открытых рядов; 0 (серое) и 7 (наследие) -> 0
    static uint8 RowsOpenForQuality(uint32 quality);
    // пул есть, качество подходит, не рубашка (InvType 4) и не накидка (19)
    static bool IsEligibleItem(ItemTemplate const* proto);

    [[nodiscard]] ItemTalents::TalentDef const* GetDef(char pool, uint8 row, uint8 choice) const;
    // все choice-id меню (pool, row); nullptr = у ряда нет вариантов в сиде
    [[nodiscard]] std::vector<uint8> const* GetMenu(char pool, uint8 row) const;
    // значение с учётом качества ролла: базовая формула * QualityMults[quality]
    [[nodiscard]] int32 CalcValue(ItemTalents::TalentDef const& def, uint32 itemLevel,
        uint8 quality) const;

    // ---- кэш состояния ----
    void LoadPlayerState(Player* player);   // два SELECT по owner_guid, идемпотентно
    void UnloadPlayerState(Player* player); // на логауте, ПОСЛЕ FlushKills
    ItemTalents::ItemState* GetState(ObjectGuid::LowType ownerGuid, ObjectGuid::LowType itemGuid);
    // кэш игрока -> точечный SELECT по item_guid (предмет мог сменить владельца)
    // -> нулевое состояние; в конце лениво роллит слоты (EnsureRolled, не для ботов)
    ItemTalents::ItemState& EnsureState(Player* player, Item* item);
    // Ленивый ролл (вызывается из EnsureState, состояние уже в кэше): для
    // каждого ряда с меню - 3 РАЗЛИЧНЫХ случайных choice + качество по весам;
    // старый выбор (rowN до смены семантики) закрепляется в одноимённом слоте
    // с качеством 0. Персист одним batched INSERT в item_talent_rolls.
    void EnsureRolled(Player* player, Item* item);

    // ---- очки ----
    [[nodiscard]] uint32 EarnedPoints(uint32 kills) const;
    static uint32 SpentPoints(ItemTalents::ItemState const& state);
    [[nodiscard]] uint32 FreePoints(ItemTalents::ItemState const& state) const;
    // кумулятивный порог следующего очка; 0 = все очки уже заработаны
    [[nodiscard]] uint32 NextPointNeed(uint32 kills) const;

    // ---- опыт предмета ----
    void AddKill(Player* player);   // +1 каждому надетому подходящему предмету (in-memory)
    void FlushKills(Player* player); // один batched INSERT ... ON DUPLICATE KEY UPDATE

    // ---- применение статов ----
    // выбранный СЛОТ -> ролл (choice, quality) -> def(pool,row,choice) -> значение
    void ApplyTalent(Player* player, Item* item, uint8 row, uint8 slot, bool apply);
    void ApplyAllTalents(Player* player, Item* item, bool apply); // хук OnPlayerApplyItemMods

    // ---- выбор/сброс: БД (sync) + кэш; эффект применяет/снимает вызывающий ----
    void SaveChoice(Player* player, Item* item, uint8 row, uint8 slot);
    void ResetChoice(Player* player, Item* item, uint8 row);

    [[nodiscard]] bool IsNearMaster(Player* player) const;

private:
    ItemTalentsMgr() = default;

    // choice 0 зарезервирован под ключ меню (pool, row) в _menus
    static uint32 MakeKey(char pool, uint8 row, uint8 choice)
    {
        return (uint32(uint8(pool)) << 16) | (uint32(row) << 8) | choice;
    }

    void ApplyEffect(Player* player, std::string const& effect, int32 value, bool apply) const;
    [[nodiscard]] uint8 RollQuality() const; // 0..2 по весам ItemTalents.QualityWeights

    bool _enable = false;
    bool _defsLoaded = false;
    bool _ignoreBots = true;
    uint8 _maxImplementedRow = 2;
    float _masterRange = 10.0f;
    uint32 _eliteMultiplier = 1;        // задел (DESIGN §2 "Опыт предмета"), в v1 не используется
    std::vector<uint32> _cumThresholds; // кумулятивные пороги очков (50,200,600,1600,4100)
    std::vector<uint32> _masterEntries;
    // качество ролла: веса выпадения и множители значения (индекс = quality 0..2)
    std::array<double, ItemTalents::NUM_QUALITIES> _qualityChances = { 70.0, 25.0, 5.0 };
    std::array<float, ItemTalents::NUM_QUALITIES> _qualityMults = { 1.0f, 1.25f, 1.5f };

    std::unordered_map<uint32, ItemTalents::TalentDef> _defs; // MakeKey -> def
    std::unordered_map<uint32, std::vector<uint8>> _menus;    // MakeKey(pool,row,0) -> choice-id

    using OwnerStates = std::unordered_map<ObjectGuid::LowType, ItemTalents::ItemState>;
    // ownerGuidLow -> itemGuidLow -> state; наличие ключа владельца = "кэш загружен"
    std::unordered_map<ObjectGuid::LowType, OwnerStates> _states;
};

#define sItemTalentsMgr ItemTalentsMgr::instance()

#endif // MOD_ITEM_TALENTS_MGR_H
