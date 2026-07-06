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
 * Ряды 3-4: процентные ауры (server-side spell_dbc 108900-108914),
 * пропуск износа, бонус урона по немезидам, бонус статов при фамильяре,
 * золото/опыт - агрегируются по игроку в PlayerPerks (см. ниже).
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
#include <unordered_set>
#include <vector>

class Creature;
class Item;
class Player;
class Unit;
struct ItemTemplate;

namespace ItemTalents
{
    constexpr uint8 MAX_ROWS = 5;
    constexpr uint8 MAX_SLOTS = 3;         // роллы-слоты ряда (UI: выбор 1 из 3)
    constexpr uint8 MAX_MENU_CHOICES = 15; // потолок choice в сиде item_talent_def
    constexpr uint8 NUM_QUALITIES = 3;     // Обычный / Отличный / Совершенный

    // Ауры процентных перков ряда 3 (server-side spell_dbc, миграция
    // data/sql/updates/pending_db_world/mod_item_talents_aura_spells.sql).
    // По 5 рангов 1..5%; на игроке держится максимум ОДИН ранг на эффект.
    constexpr uint8  AURA_RANKS = 5;
    constexpr uint32 SPELL_MOVE_SPEED_R1 = 108900; // 108900-108904, аура 129 MOD_SPEED_ALWAYS
    constexpr uint32 SPELL_HEAL_TAKEN_R1 = 108905; // 108905-108909, аура 118 MOD_HEALING_PCT
    constexpr uint32 SPELL_PHYS_TAKEN_R1 = 108910; // 108910-108914, аура 87 MOD_DAMAGE_PERCENT_TAKEN (физ.)

    // Визуал прока JORDAN_LIGHTNING: классический NPC-спелл "Lightning Bolt"
    // (природа, SPELL_EFFECT_SCHOOL_DAMAGE; в живой БД его кастуют Riverpaw
    // Mystic / Murloc Minor Oracle через SAI). Урон переопределяется
    // basepoints, каст triggered - визуал и звук молнии бесплатно.
    constexpr uint32 SPELL_GENERIC_LIGHTNING_BOLT = 9532;

    // Строка item_talent_def (acore_world)
    struct TalentDef
    {
        std::string nameRu;
        std::string descRu;
        std::string effect;   // STAT_STA, RATING_CRIT, MP5, ...
        float base = 0.0f;
        float perIlvl = 0.0f; // > 0: флэт = max(1, ceil(base + perIlvl * ilvl)); 0: процент в base
    };

    // Именной перк ряда 5 (строка item_talent_named, acore_world):
    // TalentDef + прок-поля. proc_chance = 0 -> пассив (MANA_FLAT и т.п.)
    struct NamedDef
    {
        TalentDef def;
        uint8 procChance = 0;
        uint32 icdSecs = 0;
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

    // Зарегистрированный прок именного перка (пока предмет надет)
    struct ActiveProc
    {
        ObjectGuid::LowType itemGuid = 0;
        uint32 itemEntry = 0;
        int32 value = 0;       // CalcValue именного перка
        uint8 procChance = 0;  // %
        uint32 icdMs = 0;      // внутренний кулдаун
        uint32 lastMs = 0;     // getMSTime последнего срабатывания
    };

    // Агрегат процентных перков рядов 3-4 по НАДЕТЫМ предметам игрока.
    // Ведётся инкрементально из ApplyEffect (симметрично apply/unapply),
    // капы применяются в точке использования (ауры/хуки).
    struct PlayerPerks
    {
        int32 moveSpeedPct = 0;    // MOVE_SPEED_PCT, кап Cap.MoveSpeedPct
        int32 healTakenPct = 0;    // HEAL_TAKEN_PCT, кап Cap.HealTakenPct
        int32 physTakenPct = 0;    // PHYS_TAKEN_PCT, хранится положительным
        int32 nemesisDmgPct = 0;   // NEMESIS_DMG_PCT, кап Cap.NemesisDmgPct
        int32 goldPct = 0;         // GOLD_XP_PCT (золото), кап Cap.GoldPct
        int32 xpPct = 0;           // GOLD_XP_PCT: фикс. +1% за предмет, кап Cap.XpPct
        int32 familiarStats = 0;   // FAMILIAR_ALL_STATS: желаемый флэт всех статов
        int32 familiarApplied = 0; // FAMILIAR_ALL_STATS: фактически применённый флэт
        uint32 lastSoundMs = 0;    // троттлинг PlayItemSound (getMSTime)
        std::unordered_map<ObjectGuid::LowType, uint8> duraSavePct; // itemGuid -> % пропуска износа
        std::vector<ActiveProc> coinProcs;      // JORDAN_COIN (убийства)
        std::vector<ActiveProc> lightningProcs; // JORDAN_LIGHTNING (боевые касты)
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

    // ---- именные наборы (item_talent_named, ряд 5 по item entry) ----
    [[nodiscard]] bool HasNamedSet(uint32 itemEntry) const
    {
        return _named.contains(itemEntry);
    }
    [[nodiscard]] ItemTalents::NamedDef const* GetNamedDef(uint32 itemEntry, uint8 choice) const;
    // Ряды, открытые предмету: качество + именной бонус. v1-ТЕСТОВОЕ
    // послабление: именной набор открывает ряд 5 уже на ЭПИКЕ (финальное
    // правило фазы 2 - легендарка, см. PERKS.md "Именное пробуждение").
    [[nodiscard]] uint8 RowsOpenForItem(ItemTemplate const* proto) const;
    // Ряд можно выбирать: row <= MaxImplementedRow ИЛИ именной ряд 5
    [[nodiscard]] bool IsRowSelectable(ItemTemplate const* proto, uint8 row) const;
    // Универсальный лукап определения: именной ряд 5 -> item_talent_named,
    // иначе item_talent_def (pool, row, choice)
    [[nodiscard]] ItemTalents::TalentDef const* GetDefForItem(ItemTemplate const* proto,
        uint8 row, uint8 choice) const;

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

    // Перенос состояния (item_talents + item_talent_rolls + in-memory кэш)
    // со старого GUID на новый. Вызывается из mod-gear-ascension при
    // пересоздании предмета (DestroyItem -> StoreNewItem). БД - синхронно
    // (DirectExecute), чтобы последующий точечный SELECT в EnsureState
    // гарантированно увидел перенос.
    void TransferItem(ObjectGuid::LowType oldItemGuid, ObjectGuid::LowType newItemGuid,
        Player* player);

    // ---- очки ----
    [[nodiscard]] uint32 EarnedPoints(uint32 kills) const;
    static uint32 SpentPoints(ItemTalents::ItemState const& state);
    [[nodiscard]] uint32 FreePoints(ItemTalents::ItemState const& state) const;
    // кумулятивный порог следующего очка; 0 = все очки уже заработаны
    [[nodiscard]] uint32 NextPointNeed(uint32 kills) const;
    // порог последнего очка (все 5 очков); 0 - пороги не настроены
    [[nodiscard]] uint32 MaxPointsKills() const
    {
        return _cumThresholds.empty() ? 0 : _cumThresholds.back();
    }

    // ---- опыт предмета ----
    void AddKill(Player* player);   // +1 каждому надетому подходящему предмету (in-memory)
    void FlushKills(Player* player); // один batched INSERT ... ON DUPLICATE KEY UPDATE
    // GM/тест: выставить kills предмета (кэш + синхронный UPDATE в БД)
    void SetKills(Player* player, Item* item, uint32 kills);
    // GM/тест: снять применённые таланты, обнулить выборы и роллы, зароллить заново
    void RerollItem(Player* player, Item* item);

    // ---- применение статов ----
    // выбранный СЛОТ -> ролл (choice, quality) -> def(pool,row,choice) -> значение
    void ApplyTalent(Player* player, Item* item, uint8 row, uint8 slot, bool apply);
    void ApplyAllTalents(Player* player, Item* item, bool apply); // хук OnPlayerApplyItemMods

    // ---- выбор/сброс: БД (sync) + кэш; эффект применяет/снимает вызывающий ----
    void SaveChoice(Player* player, Item* item, uint8 row, uint8 slot);
    void ResetChoice(Player* player, Item* item, uint8 row);

    // ---- общая логика выбора/сброса (команда .itemtalent + госсип мастера) ----
    // nullptr = успех; иначе код ошибки протокола аддона (NO_ITEM, ROW_LOCKED,
    // NO_POINTS, ...). TryChoose при успехе сам делает SaveChoice + ApplyTalent
    // + привязку (первый вложенный талант делает предмет персональным).
    char const* ValidateUsableItem(Player* player, Item* item) const;
    char const* TryChoose(Player* player, Item* item, uint8 row, uint8 slot,
        bool requireNearMaster);
    char const* TryReset(Player* player, Item* item, uint8 row, bool requireNearMaster);

    [[nodiscard]] bool IsNearMaster(Player* player) const;
    [[nodiscard]] bool IsMasterEntry(uint32 entry) const;

    // ---- эффекты рядов 3-4 (хуки скриптов) ----
    // DURA_SAVE: N% событий износа этого предмета пропускается (points -> 0)
    void HandleDurabilityLoss(Player* player, Item* item, int32& points);
    // GOLD_XP_PCT: суммарные бонусы с капами (0 = перк не выбран/бот)
    [[nodiscard]] uint32 GetGoldBonusPct(Player* player) const;
    [[nodiscard]] uint32 GetXpBonusPct(Player* player) const;
    // Именные проки: JORDAN_COIN (из OnPlayerCreatureKill) и
    // JORDAN_LIGHTNING (из OnPlayerSpellCast, цель-враг); оба - шанс
    // proc_chance% + per-player ICD; срабатывание зовёт PlayItemSound
    void OnKillProcs(Player* player);
    void OnCombatSpellCast(Player* player, Unit* target, uint32 spellId);
    // NEMESIS_DMG_PCT: суммарный бонус с капом; IsNemesisTarget - лучший
    // доступный признак немезиды (кэш spawnId из character_nemesis, см. cpp)
    [[nodiscard]] uint32 GetNemesisBonusPct(Player* player) const;
    [[nodiscard]] bool IsNemesisTarget(Creature* creature) const;
    void UpdateNemesisCache(uint32 diff); // из WorldScript::OnUpdate
    // FAMILIAR_ALL_STATS: пересчёт при изменении владельческих аур фамильяра
    [[nodiscard]] bool IsFamiliarOwnerAura(uint32 spellId) const;
    void OnFamiliarAuraChanged(Player* player);

    // ---- звук пробуждённого предмета ----
    // Полностью пробуждён (v1-послабление): выбраны все ряды, открытые
    // качеством, и не меньше MaxImplementedRow рядов. Финальное правило
    // фазы 2 - все 5 рядов.
    [[nodiscard]] bool IsFullyAwakened(ItemTalents::ItemState const& state,
        ItemTemplate const* proto) const;
    [[nodiscard]] uint32 GetItemSoundId(uint32 itemEntry) const;
    // Играет звук предмета владельцу, если предмет полностью пробуждён, у его
    // entry настроен звук (ItemTalents.ItemSounds) и не сработал троттлинг
    // (ItemTalents.SoundCooldown). true = звук проигран. Задел под проки фазы 2.
    bool PlayItemSound(Player* player, Item* item);
    // v1: с шансом ItemTalents.SoundOnKillChance проиграть звук первого
    // полностью пробуждённого надетого предмета со звуком (из OnCreatureKill)
    void OnKillSoundChance(Player* player);

private:
    ItemTalentsMgr() = default;

    // choice 0 зарезервирован под ключ меню (pool, row) в _menus
    static uint32 MakeKey(char pool, uint8 row, uint8 choice)
    {
        return (uint32(uint8(pool)) << 16) | (uint32(row) << 8) | choice;
    }

    // procChance/icdSecs - только для прок-эффектов именных перков
    void ApplyEffect(Player* player, Item* item, std::string const& effect, int32 value,
        bool apply, uint8 procChance = 0, uint32 icdSecs = 0);
    [[nodiscard]] uint8 RollQuality() const; // 0..2 по весам ItemTalents.QualityWeights
    // Общий раннер прок-списка: ICD + шанс -> true (обновляет lastMs)
    static bool RollProc(ItemTalents::ActiveProc& proc);

    // ---- агрегат перков ----
    ItemTalents::PlayerPerks* GetPerks(ObjectGuid::LowType ownerGuid);
    [[nodiscard]] ItemTalents::PlayerPerks const* GetPerks(ObjectGuid::LowType ownerGuid) const;
    ItemTalents::PlayerPerks& EnsurePerks(Player* player);
    // Держит на игроке ровно один ранг ауры firstSpellId..+4 по клампу
    // min(total, cap, 5); 0 - снимает все ранги
    void UpdateRankAura(Player* player, int32 totalPct, int32 capPct, uint32 firstSpellId) const;
    // Довести применённый флэт всех статов до желаемого (0, если фамильяр
    // не призван) через HandleStatFlatModifier + UpdateStatBuffMod
    void RecalcFamiliarStats(Player* player);
    [[nodiscard]] bool HasFamiliarOwnerAura(Player* player) const;

    bool _enable = false;
    bool _defsLoaded = false;
    bool _ignoreBots = true;
    uint8 _maxImplementedRow = 4;
    float _masterRange = 10.0f;
    uint32 _eliteMultiplier = 1;        // задел (DESIGN §2 "Опыт предмета"), в v1 не используется
    std::vector<uint32> _cumThresholds; // кумулятивные пороги очков (50,200,600,1600,4100)
    std::vector<uint32> _masterEntries;
    // качество ролла: веса выпадения и множители значения (индекс = quality 0..2)
    std::array<double, ItemTalents::NUM_QUALITIES> _qualityChances = { 70.0, 25.0, 5.0 };
    std::array<float, ItemTalents::NUM_QUALITIES> _qualityMults = { 1.0f, 1.25f, 1.5f };

    // Капы стакинга рядов 3-4 (PERKS.md; значения - заготовка под тюнинг).
    // ВНИМАНИЕ: эффективный потолок аурных эффектов дополнительно ограничен
    // AURA_RANKS (5%) - рангов спеллов всего 5.
    int32 _capMoveSpeedPct = 5;
    int32 _capHealTakenPct = 10;
    int32 _capPhysTakenPct = 3;
    int32 _capNemesisDmgPct = 15;
    int32 _capGoldPct = 20;
    int32 _capXpPct = 10;

    // Немезиды: кэш spawnId из acore_characters.character_nemesis
    uint32 _nemesisRefreshMs = 30000;
    uint32 _nemesisRefreshTimer = 0;
    int8 _nemesisTableStatus = 0; // 0 = не проверяли, 1 = есть, -1 = таблицы нет
    std::unordered_set<ObjectGuid::LowType> _nemesisSpawnIds;

    // Владельческие ауры фамильяров (гача): диапазоны spell id
    std::vector<std::pair<uint32, uint32>> _familiarAuraRanges;

    // Звуки пробуждённых предметов: item entry -> SoundEntries id
    std::unordered_map<uint32, uint32> _itemSounds;
    uint32 _soundCooldownMs = 30000;
    uint32 _soundOnKillChance = 5;

    std::unordered_map<uint32, ItemTalents::TalentDef> _defs; // MakeKey -> def
    std::unordered_map<uint32, std::vector<uint8>> _menus;    // MakeKey(pool,row,0) -> choice-id
    // item entry -> именные перки ряда 5 (индекс = choice - 1)
    std::unordered_map<uint32, std::array<ItemTalents::NamedDef, ItemTalents::MAX_SLOTS>> _named;

    using OwnerStates = std::unordered_map<ObjectGuid::LowType, ItemTalents::ItemState>;
    // ownerGuidLow -> itemGuidLow -> state; наличие ключа владельца = "кэш загружен"
    std::unordered_map<ObjectGuid::LowType, OwnerStates> _states;
    // ownerGuidLow -> агрегат перков рядов 3-4 (только надетое, чистится на логауте)
    std::unordered_map<ObjectGuid::LowType, ItemTalents::PlayerPerks> _perks;
};

#define sItemTalentsMgr ItemTalentsMgr::instance()

#endif // MOD_ITEM_TALENTS_MGR_H
