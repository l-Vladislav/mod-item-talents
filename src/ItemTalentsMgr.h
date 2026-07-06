/*
 * mod-item-talents: менеджер (синглтон).
 *
 * Держит кэш определений талантов (acore_world.item_talent_def), кэш
 * состояния предметов (acore_characters.item_talents) и применяет статы
 * напрямую (без spell_dbc), зеркаля вызовы ядра из Player::_ApplyItemBonuses.
 *
 * Дизайн: .claude/item-talents/DESIGN.md (особенно §2, §4, §6).
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
    constexpr uint8 MAX_CHOICES = 3;

    // Строка item_talent_def (acore_world)
    struct TalentDef
    {
        std::string nameRu;
        std::string descRu;
        std::string effect;   // STAT_STA, RATING_CRIT, MP5, ...
        float base = 0.0f;
        float perIlvl = 0.0f; // > 0: флэт = max(1, ceil(base + perIlvl * ilvl)); 0: процент в base
    };

    // Состояние одного предмета (кэш строки item_talents, acore_characters)
    struct ItemState
    {
        std::array<uint8, MAX_ROWS> rows = { }; // 0 = не выбран, 1..3
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
    static int32 CalcValue(ItemTalents::TalentDef const& def, uint32 itemLevel);

    // ---- кэш состояния ----
    void LoadPlayerState(Player* player);   // один SELECT по owner_guid, идемпотентно
    void UnloadPlayerState(Player* player); // на логауте, ПОСЛЕ FlushKills
    ItemTalents::ItemState* GetState(ObjectGuid::LowType ownerGuid, ObjectGuid::LowType itemGuid);
    // кэш игрока -> точечный SELECT по item_guid (предмет мог сменить владельца) -> нулевое состояние
    ItemTalents::ItemState& EnsureState(Player* player, Item* item);

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
    void ApplyTalent(Player* player, Item* item, uint8 row, uint8 choice, bool apply) const;
    void ApplyAllTalents(Player* player, Item* item, bool apply); // хук OnPlayerApplyItemMods

    // ---- выбор/сброс: БД (sync) + кэш; эффект применяет/снимает вызывающий ----
    void SaveChoice(Player* player, Item* item, uint8 row, uint8 choice);
    void ResetChoice(Player* player, Item* item, uint8 row);

    [[nodiscard]] bool IsNearMaster(Player* player) const;

private:
    ItemTalentsMgr() = default;

    static uint32 MakeKey(char pool, uint8 row, uint8 choice)
    {
        return (uint32(uint8(pool)) << 16) | (uint32(row) << 8) | choice;
    }

    void ApplyEffect(Player* player, std::string const& effect, int32 value, bool apply) const;

    bool _enable = false;
    bool _defsLoaded = false;
    bool _ignoreBots = true;
    uint8 _maxImplementedRow = 2;
    float _masterRange = 10.0f;
    uint32 _eliteMultiplier = 1;        // задел (DESIGN §2 "Опыт предмета"), в v1 не используется
    std::vector<uint32> _cumThresholds; // кумулятивные пороги очков (50,200,600,1600,4100)
    std::vector<uint32> _masterEntries;

    std::unordered_map<uint32, ItemTalents::TalentDef> _defs; // MakeKey -> def

    using OwnerStates = std::unordered_map<ObjectGuid::LowType, ItemTalents::ItemState>;
    // ownerGuidLow -> itemGuidLow -> state; наличие ключа владельца = "кэш загружен"
    std::unordered_map<ObjectGuid::LowType, OwnerStates> _states;
};

#define sItemTalentsMgr ItemTalentsMgr::instance()

#endif // MOD_ITEM_TALENTS_MGR_H
