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
 * Ряд 5 "Пробуждение" (фаза 2): только БАЗОВЫЕ эпики (IsBaseEpic - корень
 * GA-цепочки item_upgrade_chain имеет Quality 4) и именные предметы.
 * Каждый перк ряда 5 - прок: сид item_talent_def с effect='PROC',
 * base = id скрытого триггер-спелла (108000+), per_ilvl = коэффициент;
 * параметры прока - world-таблица item_talent_procs. Срабатывания
 * обрабатываются В МОДУЛЕ через хуки (не через ядровую прок-систему),
 * движок - ItemTalentsProcs.cpp.
 *
 * Дизайн: .claude/item-talents/DESIGN.md (особенно §2, §4, §6) и PERKS.md;
 * спеллы ряда 5 - .claude/item-talents/CUSTOM_SPELLS.md.
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
class SpellInfo;
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

    // Визуал прока JORDAN_LIGHTNING (легаси-путь до применения миграции
    // фазы 2) и болтов Фамильяра-фантома: классический NPC-спелл
    // "Lightning Bolt" (природа, SPELL_EFFECT_SCHOOL_DAMAGE). Урон
    // переопределяется basepoints, каст triggered - визуал бесплатно.
    constexpr uint32 SPELL_GENERIC_LIGHTNING_BOLT = 9532;

    // Ряд 5 "Пробуждение" (фаза 2, CUSTOM_SPELLS.md): скрытые триггер-пассивы
    // (маркеры на игроке) и видимые спеллы эффектов (spell_dbc, миграция
    // pending_db_world/mod_item_talents_proc_spells.sql).
    constexpr uint32 TRIGGER_SPELL_FIRST = 108000; // 108000-108042 заняты
    constexpr uint32 TRIGGER_SPELL_LAST  = 108044; // 108043-108044 - резерв
    constexpr uint32 VISIBLE_SPELL_FIRST = 108050;
    constexpr uint32 VISIBLE_SPELL_LAST  = 108092;
    constexpr uint32 SPELL_BLADE_DANCE   = 108066; // Танец клинка (NEXT_HIT_BONUS)

    // Триггеры проков ряда 5 (item_talent_procs.trigger_type).
    // ВАЖНО: у ядра нет хуков с исходом удара (крит/уклонение/парирование/
    // блок), а ядровую прок-систему модуль сознательно не использует.
    // *_CRIT/DODGE/PARRY/BLOCK реализованы СТАТИСТИЧЕСКОЙ аппроксимацией:
    // прок роллится на каждое событие-носитель (удар/входящий замах) с
    // шансом chance% * шанс_исхода (реальный крит/додж/парри/блок игрока
    // на момент события). Частота срабатываний совпадает с честной
    // прок-моделью, но момент не синхронизирован с фактическим исходом
    // в комбат-логе. TODO: заменить на точную детекцию, когда появится
    // ядровый хук с MeleeHitOutcome/crit-флагом.
    enum class ProcTrigger : uint8
    {
        MELEE_HIT,    // удар/автоатака игрока (авто-замах или melee-абилка)
        MELEE_CRIT,   // аппроксимация: MELEE_HIT x шанс мили-крита
        ANY_CRIT,     // аппроксимация: любой урон x соотв. шанс крита
        KILL,         // OnPlayerCreatureKill (isHonorOrXPTarget)
        DODGE,        // аппроксимация: входящий замах x шанс уклонения
        PARRY,        // аппроксимация: входящий замах x шанс парирования
        BLOCK,        // аппроксимация: входящий замах x шанс блока
        TAKEN_HIT,    // получен урон; school: 0 = любой (OnDamage),
                      // 1 = мили-замах, 126 = магия (маска школ)
        TAKEN_CRIT,   // аппроксимация: входящий замах x шанс крита атакующего
        CAST_HARMFUL, // боевой каст по врагу (OnPlayerSpellCast)
        LOW_HP,       // здоровье после удара ниже hp_threshold%
        BIG_HIT,      // один удар снял >= hp_threshold% макс. здоровья
        RANGED_HIT,   // выстрел игрока (spell с DmgClass RANGED)
        RANGED_CRIT,  // аппроксимация: RANGED_HIT x шанс рендж-крита
        SPELL_CRIT,   // аппроксимация: урон заклинанием x шанс спелл-крита
    };

    // Эффекты проков ряда 5 (item_talent_procs.effect_type)
    enum class ProcEffect : uint8
    {
        DAMAGE,         // урон цели (visible_spell = SCHOOL_DAMAGE)
        DOT,            // периодический урон (bp = значение / число тиков)
        HEAL,           // хил себе
        BUFF_SELF,      // бафф на себя (bp-override при coef > 0)
        DEBUFF_TARGET,  // дебафф цели (значение передаётся ОТРИЦАТЕЛЬНЫМ)
        ABSORB,         // щит-поглощение на себя
        EXTRA_ATTACK,   // доп. удары (SPELL_EFFECT_ADD_EXTRA_ATTACKS, bp из dbc)
        INTERRUPT,      // прерывание каста цели
        STUN,           // оглушение цели (Mechanic stun - боссы иммунны)
        AOE_DAMAGE,     // урон всем врагам в 5 м от игрока (до 8 целей)
        CLEAVE,         // урон цели и до 2 врагам рядом с ней
        ENERGIZE,       // восстановление маны себе
        SUMMON,         // Фамильяр-фантом (TempSummon, best effort)
        NEXT_HIT_BONUS, // следующий удар +N урона (Танец клинка)
    };

    // Строка item_talent_procs (acore_world): параметры прока ряда 5
    struct ProcDef
    {
        uint32 visibleSpell = 0;  // 108050+ (CUSTOM_SPELLS.md)
        ProcTrigger trigger = ProcTrigger::MELEE_HIT;
        ProcEffect effect = ProcEffect::DAMAGE;
        uint8 school = 0;         // фильтр школы ТРИГГЕРА (только TAKEN_HIT)
        uint8 chance = 0;         // %
        uint32 icdSecs = 0;
        float coef = 0.0f;        // значение = ceil(coef * ilvl); 0 = bp из dbc
        uint32 durationSecs = 0;  // справочно (длительность зашита в dbc)
        uint8 hpThreshold = 0;    // LOW_HP / BIG_HIT
    };

    // Активный прок ряда 5 (предмет надет)
    struct Row5Proc
    {
        ObjectGuid::LowType itemGuid = 0;
        uint32 itemEntry = 0;
        uint32 triggerSpell = 0; // ключ в item_talent_procs
        int32 value = 0;         // ceil(coef * ilvl), без множителей качества
    };

    // Активный Фамильяр-фантом (эффект SUMMON)
    struct Phantom
    {
        ObjectGuid owner;
        ObjectGuid creature;
        ObjectGuid target;
        int32 value = 0;         // урон болта
        int32 remainingMs = 0;
        int32 tickMs = 0;        // до следующего болта
    };

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
        // Уровни пробуждения - СЕГМЕНТЫ (решение 2026-07-07): kills - счётчик
        // убийств ВНУТРИ текущего уровня; взятие уровня ОБНУЛЯЕТ kills
        // (излишки сгорают), level растёт до MAX_ROWS.
        uint32 kills = 0;   // убийства в счёт СЛЕДУЮЩЕГО уровня
        uint8 level = 0;    // уровень пробуждения 0..5 (источник - БД, не kills)
        bool dirty = false; // kills/level изменены и ещё не записаны в БД
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
        std::vector<ActiveProc> lightningProcs; // JORDAN_LIGHTNING (легаси, боевые касты)

        // ---- ряд 5 "Пробуждение" ----
        std::vector<Row5Proc> row5Procs;        // активные проки надетых предметов
        std::unordered_map<uint32, uint32> procIcd; // triggerSpell -> lastMs; общий
                                                    // ICD на игрока (дубликат перка
                                                    // с двух предметов делит кулдаун)
        int32 nextHitBonus = 0;    // Танец клинка: бонус следующего удара
        uint32 lastMeleeDoneMs = 0; // дедуп двух вызовов ModifyMeleeDamage за замах
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
    // (class, subclass, InvType) -> пул A..H; nullopt = предмет вне системы
    // (DESIGN §4). InvType нужен для держим-в-руке (class4/subclass0/InvType23
    // -> пул A), чтобы не спутать с кольцами/тринкетами того же subclass 0.
    static std::optional<char> GetPool(uint32 itemClass, uint32 itemSubClass,
        uint32 itemInvType = 0);
    // качество 1..5+ -> 1..5 открытых рядов; 0 (серое) и 7 (наследие) -> 0
    static uint8 RowsOpenForQuality(uint32 quality);
    // пул есть, качество подходит, не рубашка (InvType 4) и не накидка (19)
    static bool IsEligibleItem(ItemTemplate const* proto);

    // Лукап дефа/меню: точное совпадение подкласса предмета, затем фолбэк на
    // subclass = -1 (ряды 1-4 и ряд 5 брони сидятся с -1; ряд 5 оружия -
    // подкласс-специфичный, DESIGN "Ряд 5" / PERKS "Ряд 5 Пробуждение")
    [[nodiscard]] ItemTalents::TalentDef const* GetDef(char pool, uint8 row, uint8 choice,
        int16 subclass = -1) const;
    // все choice-id меню (pool, row, subclass); nullptr = у ряда нет вариантов
    [[nodiscard]] std::vector<uint8> const* GetMenu(char pool, uint8 row,
        int16 subclass = -1) const;
    // значение с учётом качества ролла: базовая формула * QualityMults[quality].
    // effect='PROC': значение = max(1, ceil(per_ilvl * ilvl)), качество
    // НЕ применяется (решение PERKS "Роллы и качество"), base = spell id.
    [[nodiscard]] int32 CalcValue(ItemTalents::TalentDef const& def, uint32 itemLevel,
        uint8 quality) const;

    // ---- именные наборы (item_talent_named, ряд 5 по item entry) ----
    [[nodiscard]] bool HasNamedSet(uint32 itemEntry) const
    {
        return _named.contains(itemEntry);
    }
    [[nodiscard]] ItemTalents::NamedDef const* GetNamedDef(uint32 itemEntry, uint8 choice) const;
    // Гейт ряда 5 (фаза 2, PERKS "Ряд 5"): БАЗОВЫЙ эпик = Quality 4 И предмет
    // не GA-копия (нет в item_upgrade_chain.next_entry - эпик-базы GA не
    // апгрейдит). Все GA-копии предзагружены в _gaCopyEntries при старте,
    // проверка чисто в памяти; наличие GA-таблицы гейтится через
    // information_schema (модуль GA может отсутствовать - тогда все эпики
    // базовые).
    [[nodiscard]] bool IsBaseEpic(ItemTemplate const* proto) const;
    // Ряды, открытые предмету: качество + ряд 5 у именных наборов и БАЗОВЫХ
    // эпиков уже на эпике (решение 2026-07-06, PERKS "Ряд 5"). Не-базовые
    // эпики - потолок 4 ряда.
    [[nodiscard]] uint8 RowsOpenForItem(ItemTemplate const* proto) const;
    // Ряд можно выбирать: row <= MaxImplementedRow ИЛИ ряд 5 именного набора /
    // базового эпика (проки требуют загруженного item_talent_procs)
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

    // Хеш перк-состояния НАДЕТЫХ eligible-предметов (FNV-1a 32): слот, guid,
    // уровень пробуждения, выбранные ряды. Kills сознательно НЕ входят
    // (меняются каждым убийством - кэш аддона инвалидировался бы любым боем);
    // протокол .itemtalent sync досылает их отдельной строкой. Назначение -
    // быстрая верификация локального кэша аддона при логине: совпало - кэш
    // свежий и нетронутый, нет - полный list.
    [[nodiscard]] uint32 ComputePerkHash(Player* player);

    // Перенос состояния (item_talents + item_talent_rolls + in-memory кэш)
    // со старого GUID на новый. Вызывается из mod-gear-ascension при
    // пересоздании предмета (DestroyItem -> StoreNewItem). БД - синхронно
    // (DirectExecute), чтобы последующий точечный SELECT в EnsureState
    // гарантированно увидел перенос.
    void TransferItem(ObjectGuid::LowType oldItemGuid, ObjectGuid::LowType newItemGuid,
        Player* player);

    // ---- уровни пробуждения (сегменты, решение 2026-07-07) ----
    // Источник уровня - state.level (колонка item_talents.level), НЕ kills.
    // Сегмент убийств за уровень level 1..5 (0, если уровня нет в конфиге)
    [[nodiscard]] uint32 GetLevelSegment(uint8 level) const;
    // сегмент СЛЕДУЮЩЕГО уровня (нужно kills для level + 1); 0 = уровень 5
    [[nodiscard]] uint32 NextLevelNeed(uint8 level) const;
    static uint32 SpentPoints(ItemTalents::ItemState const& state);

    // ---- опыт предмета ----
    // +1 каждому надетому подходящему предмету (in-memory); при kills >=
    // сегмент(level+1): kills = 0, ++level (излишки сгорают) + немедленная
    // запись этого предмета в БД (редкое событие, надёжность)
    void AddKill(Player* player);
    // один batched INSERT ... ON DUPLICATE KEY UPDATE с АБСОЛЮТНЫМИ kills и
    // level (не дельтой: сброс kills на уровне ломает дельта-модель; гонок
    // нет - kills предмета меняет только его владелец онлайн)
    void FlushKills(Player* player);
    // GM/тест: выставить kills ТЕКУЩЕГО уровня (кэш + синхронный UPDATE в БД);
    // уровень не меняет
    void SetKills(Player* player, Item* item, uint32 kills);
    // GM/тест: выставить уровень пробуждения (kills = 0, кэш + синхронный UPDATE)
    void SetLevel(Player* player, Item* item, uint8 level);
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
    // Проки убийств (ряд 5 KILL + именной JORDAN_COIN, из
    // OnPlayerCreatureKill) и боевых кастов (ряд 5 CAST_HARMFUL + легаси
    // JORDAN_LIGHTNING, из OnPlayerSpellCast); шанс + per-player ICD;
    // срабатывание зовёт PlayItemSound
    void OnKillProcs(Player* player);
    void OnCombatSpellCast(Player* player, Unit* target, uint32 spellId);

    // ---- ряд 5 "Пробуждение": движок проков (ItemTalentsProcs.cpp) ----
    // Параметры проков из item_talent_procs (вызывается из LoadDefinitions)
    void LoadProcs();
    [[nodiscard]] bool ProcsLoaded() const { return _procsLoaded; }
    [[nodiscard]] ItemTalents::ProcDef const* GetProcDef(uint32 triggerSpell) const;
    // шанс прока для {chance} в desc_ru; -1 = не прок / параметров нет
    [[nodiscard]] int32 GetProcChance(uint32 triggerSpell) const;
    // Регистрация выбранного прока (effect='PROC'): запись в PlayerPerks +
    // скрытая аура-МАРКЕР triggerSpell на игроке (spell_dbc 108000+,
    // пассив/скрыта/NO_AURA_CANCEL); сами срабатывания - хуки ниже
    void ApplyProc(Player* player, Item* item, uint32 triggerSpell, int32 value, bool apply);
    // Диспетчеры событий (PvE-only: вторая сторона - существо не под
    // контролем игрока). Модель аппроксимаций - см. ProcTrigger.
    void OnProcMeleeDone(Player* player, Unit* victim, uint32& damage); // + Танец клинка
    void OnProcMeleeTaken(Player* player, Unit* attacker);
    void OnProcSpellDone(Player* player, Unit* victim, SpellInfo const* spellInfo);
    void OnProcSpellTaken(Player* player, Unit* attacker, SpellInfo const* spellInfo);
    void OnProcAnyDamageTaken(Player* player, Unit* attacker, uint32 damage);
    // Тики Фамильяра-фантома (из WorldScript::OnUpdate)
    void UpdatePhantoms(uint32 diff);
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

    // choice 0 зарезервирован под ключ меню (pool, row, subclass) в _menus;
    // subclass -1 (любой) кодируется как 0, конкретный подкласс - subclass+1
    static uint32 MakeKey(char pool, uint8 row, uint8 choice, int16 subclass = -1)
    {
        return (uint32(uint8(pool)) << 24) | (uint32(uint8(subclass + 1)) << 16)
            | (uint32(row) << 8) | choice;
    }

    // procChance/icdSecs - только для прок-эффектов именных перков
    void ApplyEffect(Player* player, Item* item, std::string const& effect, int32 value,
        bool apply, uint8 procChance = 0, uint32 icdSecs = 0);
    [[nodiscard]] uint8 RollQuality() const; // 0..2 по весам ItemTalents.QualityWeights
    // Общий раннер прок-списка: ICD + шанс -> true (обновляет lastMs)
    static bool RollProc(ItemTalents::ActiveProc& proc);

    // ---- ряд 5: внутренности движка (ItemTalentsProcs.cpp) ----
    // Вторая сторона события - валидная PvE-цель (существо не под игроком)
    static bool IsPveUnit(Unit* unit);
    // Общий диспетчер: перебор row5Procs игрока по типу триггера; chanceMult -
    // множитель аппроксимации (шанс крита/доджа/...); schoolMask - канал
    // TAKEN_HIT (0 = "любой урон", иначе маска школ события); eventDamage -
    // фактический урон события (LOW_HP / BIG_HIT)
    void HandleProcEvent(Player* player, ItemTalents::ProcTrigger trigger, Unit* target,
        float chanceMult = 1.0f, uint32 schoolMask = 0, uint32 eventDamage = 0);
    // ICD (общий на игрока по триггер-спеллу) + шанс -> true
    bool RollRow5Proc(ItemTalents::PlayerPerks& perks, uint32 triggerSpell,
        ItemTalents::ProcDef const& def, float chanceMult);
    // Исполнение эффекта: CastCustomSpell видимого спелла (bp-override при
    // coef > 0) / TempSummon фантома / прочее; в конце PlayItemSound
    void ExecuteProc(Player* player, Unit* target, ItemTalents::Row5Proc const& proc,
        ItemTalents::ProcDef const& def);
    void SummonPhantom(Player* player, Unit* target, ItemTalents::Row5Proc const& proc,
        ItemTalents::ProcDef const& def);

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
    // Сегменты убийств ЗА уровень 1..5 (50,150,400,1000,2500) - НЕ кумулятивные
    // (решение 2026-07-07): взятие уровня обнуляет счётчик kills предмета
    std::vector<uint32> _levelSegments;
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
    std::unordered_map<uint32, std::vector<uint8>> _menus;    // MakeKey(pool,row,0,sub) -> choice-id
    // item entry -> именные перки ряда 5 (индекс = choice - 1)
    std::unordered_map<uint32, std::array<ItemTalents::NamedDef, ItemTalents::MAX_SLOTS>> _named;

    // ---- ряд 5 "Пробуждение" ----
    bool _procsLoaded = false;
    uint32 _phantomCreature = 191090; // ItemTalents.PhantomCreature (дух-фамильяр)
    std::unordered_map<uint32, ItemTalents::ProcDef> _procs; // triggerSpell -> параметры
    std::vector<ItemTalents::Phantom> _phantoms;             // активные фантомы
    bool _inProc = false; // ре-энтри гард: наш каст не порождает новые проки
    // Гейт базовых эпиков: множество ВСЕХ GA-копий (next_entry из
    // item_upgrade_chain), предзагружается целиком в LoadDefinitions одним
    // запросом - IsBaseEpic на горячих путях (тултипы, экипировка, протокол
    // аддона) в БД не ходит. Таблица статична в аптайме (пишется генераторами
    // GA офлайн). Статус: 1 = загружено, -1 = таблицы нет (все эпики базовые).
    std::unordered_set<uint32> _gaCopyEntries;
    int8 _gaTableStatus = 0;

    // Ручные диапазоны entry для ряда 5 (конфиг, стиль ahbot); deny сильнее
    static bool EntryInRanges(std::vector<std::pair<uint32, uint32>> const& ranges, uint32 entry);
    std::vector<std::pair<uint32, uint32>> _row5Allow;
    std::vector<std::pair<uint32, uint32>> _row5Deny;

    using OwnerStates = std::unordered_map<ObjectGuid::LowType, ItemTalents::ItemState>;
    // ownerGuidLow -> itemGuidLow -> state; наличие ключа владельца = "кэш загружен"
    std::unordered_map<ObjectGuid::LowType, OwnerStates> _states;
    // ownerGuidLow -> агрегат перков рядов 3-4 (только надетое, чистится на логауте)
    std::unordered_map<ObjectGuid::LowType, ItemTalents::PlayerPerks> _perks;
};

#define sItemTalentsMgr ItemTalentsMgr::instance()

#endif // MOD_ITEM_TALENTS_MGR_H
