/*
 * mod-item-talents: движок проков ряда 5 "Пробуждение" (фаза 2).
 *
 * Модель (см. ItemTalentsMgr.h, DESIGN "Ряд 5", PERKS "Ряд 5 - Пробуждение"):
 *  - выбранный перк ряда 5 регистрируется в PlayerPerks.row5Procs и вешает
 *    на игрока СКРЫТУЮ постоянную ауру-МАРКЕР (spell_dbc 108000+, дамми-
 *    пассив, NO_AURA_CANCEL, спрятана с панели баффов);
 *  - срабатывания обрабатываются ЗДЕСЬ через хуки модуля (ModifyMeleeDamage /
 *    ModifySpellDamageTaken / OnDamage / OnPlayerCreatureKill /
 *    OnPlayerSpellCast), НЕ через ядровую прок-систему;
 *  - ICD - общий на игрока по триггер-спеллу (PlayerPerks.procIcd): дубликат
 *    перка с двух предметов (плащ+роба пула A) делит кулдаун;
 *  - эффект - CastCustomSpell видимого спелла 108050-108092 с bp-override
 *    ceil(coef x ilvl) (coef 0 = фиксированные basepoints из dbc);
 *  - только PvE: вторая сторона события - существо не под контролем игрока.
 *
 * АППРОКСИМАЦИЯ исходов (крит/уклонение/парирование/блок): у ядра нет
 * пост-исходных хуков (ModifyMeleeDamage зовётся ДО RollMeleeOutcomeAgainst,
 * Unit.cpp ~1748/1767), поэтому *_CRIT/DODGE/PARRY/BLOCK роллятся на каждое
 * событие-носитель с шансом chance% x фактический_шанс_исхода
 * (GetUnitCriticalChance / GetUnitDodgeChance / ...). Частота срабатываний
 * статистически совпадает с честной прок-моделью, момент - нет.
 * TODO: заменить на точную детекцию при появлении ядрового хука с исходом.
 */

#include "ItemTalentsMgr.h"
#include "CellImpl.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Item.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Optional.h"
#include "Player.h"
#include "Random.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TemporarySummon.h"
#include "Timer.h"
#include "Unit.h"
#include <algorithm>
#include <cmath>

using ItemTalents::PlayerPerks;
using ItemTalents::ProcDef;
using ItemTalents::ProcEffect;
using ItemTalents::ProcTrigger;
using ItemTalents::Row5Proc;

namespace
{
    // Дублирует хелпер из ItemTalentsMgr.cpp (там - анонимный namespace).
    // Известная грабля: SELECT из несуществующей таблицы абортит worldserver.
    bool ProcsTableExists()
    {
        return static_cast<bool>(WorldDatabase.Query(
            "SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE() "
            "AND table_name = 'item_talent_procs' LIMIT 1"));
    }

    Optional<ProcTrigger> ParseTrigger(std::string const& text)
    {
        if (text == "MELEE_HIT")    return ProcTrigger::MELEE_HIT;
        if (text == "MELEE_CRIT")   return ProcTrigger::MELEE_CRIT;
        if (text == "ANY_CRIT")     return ProcTrigger::ANY_CRIT;
        if (text == "KILL")         return ProcTrigger::KILL;
        if (text == "DODGE")        return ProcTrigger::DODGE;
        if (text == "PARRY")        return ProcTrigger::PARRY;
        if (text == "BLOCK")        return ProcTrigger::BLOCK;
        if (text == "TAKEN_HIT")    return ProcTrigger::TAKEN_HIT;
        if (text == "TAKEN_CRIT")   return ProcTrigger::TAKEN_CRIT;
        if (text == "CAST_HARMFUL") return ProcTrigger::CAST_HARMFUL;
        if (text == "LOW_HP")       return ProcTrigger::LOW_HP;
        if (text == "BIG_HIT")      return ProcTrigger::BIG_HIT;
        if (text == "RANGED_HIT")   return ProcTrigger::RANGED_HIT;
        if (text == "RANGED_CRIT")  return ProcTrigger::RANGED_CRIT;
        if (text == "SPELL_CRIT")   return ProcTrigger::SPELL_CRIT;
        return std::nullopt;
    }

    Optional<ProcEffect> ParseEffect(std::string const& text)
    {
        if (text == "DAMAGE")         return ProcEffect::DAMAGE;
        if (text == "DOT")            return ProcEffect::DOT;
        if (text == "HEAL")           return ProcEffect::HEAL;
        if (text == "BUFF_SELF")      return ProcEffect::BUFF_SELF;
        if (text == "DEBUFF_TARGET")  return ProcEffect::DEBUFF_TARGET;
        if (text == "ABSORB")         return ProcEffect::ABSORB;
        if (text == "EXTRA_ATTACK")   return ProcEffect::EXTRA_ATTACK;
        if (text == "INTERRUPT")      return ProcEffect::INTERRUPT;
        if (text == "STUN")           return ProcEffect::STUN;
        if (text == "AOE_DAMAGE")     return ProcEffect::AOE_DAMAGE;
        if (text == "CLEAVE")         return ProcEffect::CLEAVE;
        if (text == "ENERGIZE")       return ProcEffect::ENERGIZE;
        if (text == "SUMMON")         return ProcEffect::SUMMON;
        if (text == "NEXT_HIT_BONUS") return ProcEffect::NEXT_HIT_BONUS;
        return std::nullopt;
    }

    // Собственные спеллы движка не порождают новые проки (рекурсия)
    bool IsOwnProcSpell(uint32 spellId)
    {
        return spellId == ItemTalents::SPELL_GENERIC_LIGHTNING_BOLT
            || (spellId >= ItemTalents::TRIGGER_SPELL_FIRST
                && spellId <= ItemTalents::VISIBLE_SPELL_LAST);
    }

    // До maxTargets живых PvE-целей в радиусе от center (кроме exclude)
    std::vector<Unit*> CollectPveTargets(Player* player, WorldObject* center, float radius,
        uint8 maxTargets, Unit* exclude)
    {
        std::vector<Unit*> result;
        std::list<Unit*> nearby;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(center, player, radius);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(
            player, nearby, check);
        // В playerbots-форке грид-обход называется VisitObjects (не VisitAllObjects)
        Cell::VisitObjects(center, searcher, radius);

        for (Unit* unit : nearby)
        {
            if (unit == exclude || !unit->IsAlive() || !unit->IsCreature()
                || unit->IsControlledByPlayer())
                continue;

            result.push_back(unit);
            if (result.size() >= maxTargets)
                break;
        }
        return result;
    }
}

// ---------------------------------------------------------------------------
// Загрузка параметров (item_talent_procs)
// ---------------------------------------------------------------------------

void ItemTalentsMgr::LoadProcs()
{
    _procs.clear();
    _phantoms.clear();
    _procsLoaded = false;

    if (!_enable)
        return;

    if (!ProcsTableExists())
    {
        LOG_WARN("module", "mod-item-talents: acore_world.item_talent_procs is missing - "
            "row-5 awakening procs disabled. Apply "
            "pending_db_world/mod_item_talents_subclass.sql.");
        return;
    }

    QueryResult result = WorldDatabase.Query(
        "SELECT trigger_spell, visible_spell, trigger_type, effect_type, school, chance, "
        "icd_secs, coef, duration_secs, hp_threshold FROM item_talent_procs");
    if (!result)
    {
        LOG_WARN("module", "mod-item-talents: item_talent_procs is empty - row-5 procs "
            "disabled.");
        return;
    }

    do
    {
        Field* fields = result->Fetch();
        uint32 const triggerSpell = fields[0].Get<uint32>();
        std::string const triggerText = fields[2].Get<std::string>();
        std::string const effectText = fields[3].Get<std::string>();

        Optional<ProcTrigger> trigger = ParseTrigger(triggerText);
        Optional<ProcEffect> effect = ParseEffect(effectText);
        if (!triggerSpell || !trigger || !effect)
        {
            LOG_WARN("module", "mod-item-talents: skipped bad item_talent_procs row "
                "(trigger_spell {}, trigger_type '{}', effect_type '{}').",
                triggerSpell, triggerText, effectText);
            continue;
        }

        ProcDef& def = _procs[triggerSpell];
        def.visibleSpell = fields[1].Get<uint32>();
        def.trigger = *trigger;
        def.effect = *effect;
        def.school = fields[4].Get<uint8>();
        def.chance = fields[5].Get<uint8>();
        def.icdSecs = fields[6].Get<uint32>();
        def.coef = fields[7].Get<float>();
        def.durationSecs = fields[8].Get<uint32>();
        def.hpThreshold = fields[9].Get<uint8>();
    } while (result->NextRow());

    _procsLoaded = !_procs.empty();
    LOG_INFO("module", "mod-item-talents: loaded {} row-5 proc definitions.", _procs.size());
}

ProcDef const* ItemTalentsMgr::GetProcDef(uint32 triggerSpell) const
{
    auto itr = _procs.find(triggerSpell);
    return itr != _procs.end() ? &itr->second : nullptr;
}

int32 ItemTalentsMgr::GetProcChance(uint32 triggerSpell) const
{
    ProcDef const* def = GetProcDef(triggerSpell);
    return def ? int32(def->chance) : -1;
}

// ---------------------------------------------------------------------------
// Регистрация выбранного прока (из ApplyTalent, симметрично apply/unapply)
// ---------------------------------------------------------------------------

void ItemTalentsMgr::ApplyProc(Player* player, Item* item, uint32 triggerSpell, int32 value,
    bool apply)
{
    if (!player || !item)
        return;

    if (!GetProcDef(triggerSpell))
    {
        LOG_WARN("module", "mod-item-talents: no item_talent_procs row for trigger spell "
            "{} (item entry {}) - perk is inert.", triggerSpell, item->GetEntry());
        return;
    }

    PlayerPerks& perks = EnsurePerks(player);
    ObjectGuid::LowType const itemGuid = item->GetGUID().GetCounter();
    if (apply)
    {
        Row5Proc proc;
        proc.itemGuid = itemGuid;
        proc.itemEntry = item->GetEntry();
        proc.triggerSpell = triggerSpell;
        proc.value = value;
        perks.row5Procs.push_back(proc);

        // Скрытая постоянная аура-маркер (108000+): пассив, NO_AURA_CANCEL,
        // спрятана с панели. Спелла нет в spell_dbc - LoadDefinitions уже
        // предупредил, движок работает и без маркера.
        if (sSpellMgr->GetSpellInfo(triggerSpell) && !player->HasAura(triggerSpell))
            player->AddAura(triggerSpell, player);
    }
    else
    {
        std::erase_if(perks.row5Procs, [itemGuid, triggerSpell](Row5Proc const& proc)
        {
            return proc.itemGuid == itemGuid && proc.triggerSpell == triggerSpell;
        });

        // Маркер держится, пока остаётся хоть один предмет с этим проком
        bool const remains = std::any_of(perks.row5Procs.begin(), perks.row5Procs.end(),
            [triggerSpell](Row5Proc const& proc)
            {
                return proc.triggerSpell == triggerSpell;
            });
        if (!remains)
            player->RemoveAurasDueToSpell(triggerSpell);
    }
}

// ---------------------------------------------------------------------------
// Диспетчеры событий (вызываются из хуков ItemTalentsScripts.cpp)
// ---------------------------------------------------------------------------

bool ItemTalentsMgr::IsPveUnit(Unit* unit)
{
    // Только PvE (решение 2026-07-06): существо, не под контролем игрока
    // (питомцы/стражи/чармы игроков и сами игроки-боты вне системы)
    return unit && unit->IsCreature() && !unit->IsControlledByPlayer();
}

// Замах игрока (ModifyMeleeDamage, attacker = player). ДО исхода броска:
// сюда попадают и замахи, которые будут уклонены/парированы - см. шапку.
void ItemTalentsMgr::OnProcMeleeDone(Player* player, Unit* victim, uint32& damage)
{
    if (!IsEnabled() || _inProc || !player)
        return;

    PlayerPerks* perks = GetPerks(player->GetGUID().GetCounter());
    if (!perks)
        return;

    // Танец клинка (NEXT_HIT_BONUS): потребляем бонус ПЕРЕД роллом новых
    // проков. Бафф 108066 истёк без удара - бонус сгорает.
    if (perks->nextHitBonus)
    {
        if (!player->HasAura(ItemTalents::SPELL_BLADE_DANCE))
            perks->nextHitBonus = 0;
        else if (IsPveUnit(victim))
        {
            damage += uint32(perks->nextHitBonus);
            perks->nextHitBonus = 0;
            player->RemoveAurasDueToSpell(ItemTalents::SPELL_BLADE_DANCE);
        }
    }

    if (!_procsLoaded || perks->row5Procs.empty() || !IsPveUnit(victim))
        return;

    // ModifyMeleeDamage зовётся до двух раз за замах игрока (два damage-слота
    // оружия, Unit::CalculateMeleeDamage) - дедупим по таймстемпу
    uint32 const now = getMSTime();
    if (perks->lastMeleeDoneMs == now)
        return;
    perks->lastMeleeDoneMs = now;

    HandleProcEvent(player, ProcTrigger::MELEE_HIT, victim);

    float const crit = player->GetUnitCriticalChance(BASE_ATTACK, victim) * 0.01f;
    if (crit > 0.0f)
    {
        HandleProcEvent(player, ProcTrigger::MELEE_CRIT, victim, crit);
        HandleProcEvent(player, ProcTrigger::ANY_CRIT, victim, crit);
    }
}

// Входящий мили-замах существа (ModifyMeleeDamage, target = player)
void ItemTalentsMgr::OnProcMeleeTaken(Player* player, Unit* attacker)
{
    if (!IsEnabled() || !_procsLoaded || _inProc || !player || !IsPveUnit(attacker))
        return;

    PlayerPerks* perks = GetPerks(player->GetGUID().GetCounter());
    if (!perks || perks->row5Procs.empty())
        return;

    // "По вам удар (в ближнем бою)": Разряд / Шипы стали / Глухая защита
    HandleProcEvent(player, ProcTrigger::TAKEN_HIT, attacker, 1.0f, SPELL_SCHOOL_MASK_NORMAL);

    // Аппроксимации исходов входящего замаха (см. шапку файла)
    if (float const dodge = player->GetUnitDodgeChance() * 0.01f; dodge > 0.0f)
        HandleProcEvent(player, ProcTrigger::DODGE, attacker, dodge);
    if (float const parry = player->GetUnitParryChance() * 0.01f; parry > 0.0f)
        HandleProcEvent(player, ProcTrigger::PARRY, attacker, parry);
    if (float const block = player->GetUnitBlockChance() * 0.01f; block > 0.0f)
        HandleProcEvent(player, ProcTrigger::BLOCK, attacker, block);
    if (float const crit = attacker->GetUnitCriticalChance(BASE_ATTACK, player) * 0.01f;
        crit > 0.0f)
        HandleProcEvent(player, ProcTrigger::TAKEN_CRIT, attacker, crit);
}

// Урон игрока спеллом (ModifySpellDamageTaken, attacker = player):
// мили-абилки, выстрелы и заклинания - по DmgClass
void ItemTalentsMgr::OnProcSpellDone(Player* player, Unit* victim, SpellInfo const* spellInfo)
{
    if (!IsEnabled() || !_procsLoaded || _inProc || !player || !spellInfo
        || IsOwnProcSpell(spellInfo->Id) || !IsPveUnit(victim))
        return;

    PlayerPerks* perks = GetPerks(player->GetGUID().GetCounter());
    if (!perks || perks->row5Procs.empty())
        return;

    switch (spellInfo->DmgClass)
    {
        case SPELL_DAMAGE_CLASS_MELEE:
        {
            // Мили-абилки считаются "ударом" наравне с автоатакой
            HandleProcEvent(player, ProcTrigger::MELEE_HIT, victim);
            float const crit = player->GetUnitCriticalChance(BASE_ATTACK, victim) * 0.01f;
            if (crit > 0.0f)
            {
                HandleProcEvent(player, ProcTrigger::MELEE_CRIT, victim, crit);
                HandleProcEvent(player, ProcTrigger::ANY_CRIT, victim, crit);
            }
            break;
        }
        case SPELL_DAMAGE_CLASS_RANGED:
        {
            HandleProcEvent(player, ProcTrigger::RANGED_HIT, victim);
            float const crit = player->GetUnitCriticalChance(RANGED_ATTACK, victim) * 0.01f;
            if (crit > 0.0f)
            {
                HandleProcEvent(player, ProcTrigger::RANGED_CRIT, victim, crit);
                HandleProcEvent(player, ProcTrigger::ANY_CRIT, victim, crit);
            }
            break;
        }
        case SPELL_DAMAGE_CLASS_MAGIC:
        {
            float const crit = player->SpellDoneCritChance(victim, spellInfo,
                spellInfo->GetSchoolMask(), BASE_ATTACK, false) * 0.01f;
            if (crit > 0.0f)
            {
                HandleProcEvent(player, ProcTrigger::SPELL_CRIT, victim, crit);
                HandleProcEvent(player, ProcTrigger::ANY_CRIT, victim, crit);
            }
            break;
        }
        default:
            break;
    }
}

// Полученный игроком урон спеллом (ModifySpellDamageTaken, target = player)
void ItemTalentsMgr::OnProcSpellTaken(Player* player, Unit* attacker, SpellInfo const* spellInfo)
{
    if (!IsEnabled() || !_procsLoaded || _inProc || !player || !spellInfo
        || IsOwnProcSpell(spellInfo->Id) || !IsPveUnit(attacker))
        return;

    PlayerPerks* perks = GetPerks(player->GetGUID().GetCounter());
    if (!perks || perks->row5Procs.empty())
        return;

    uint32 const schoolMask = spellInfo->GetSchoolMask();
    if (schoolMask & SPELL_SCHOOL_MASK_MAGIC)
        HandleProcEvent(player, ProcTrigger::TAKEN_HIT, attacker, 1.0f,
            schoolMask & SPELL_SCHOOL_MASK_MAGIC); // "получен маг. урон" (Заземление)
    else
        HandleProcEvent(player, ProcTrigger::TAKEN_HIT, attacker, 1.0f,
            SPELL_SCHOOL_MASK_NORMAL); // физ. спелл-удар моба = "по вам удар"
}

// Любой фактический урон по игроку (UnitScript::OnDamage, Unit::DealDamage -
// после митигации, ДО списания здоровья): канал "получен урон" (school 0),
// LOW_HP и BIG_HIT по честным цифрам
void ItemTalentsMgr::OnProcAnyDamageTaken(Player* player, Unit* attacker, uint32 damage)
{
    if (!IsEnabled() || !_procsLoaded || _inProc || !player || !damage || !IsPveUnit(attacker))
        return;

    PlayerPerks* perks = GetPerks(player->GetGUID().GetCounter());
    if (!perks || perks->row5Procs.empty())
        return;

    HandleProcEvent(player, ProcTrigger::TAKEN_HIT, attacker, 1.0f, 0);
    HandleProcEvent(player, ProcTrigger::LOW_HP, attacker, 1.0f, 0, damage);
    HandleProcEvent(player, ProcTrigger::BIG_HIT, attacker, 1.0f, 0, damage);
}

// ---------------------------------------------------------------------------
// Ядро движка: перебор активных проков, ICD + шанс, исполнение
// ---------------------------------------------------------------------------

bool ItemTalentsMgr::RollRow5Proc(PlayerPerks& perks, uint32 triggerSpell, ProcDef const& def,
    float chanceMult)
{
    if (!def.chance)
        return false;

    uint32 const now = getMSTime();
    uint32& last = perks.procIcd[triggerSpell];
    if (last && def.icdSecs && getMSTimeDiff(last, now) < def.icdSecs * IN_MILLISECONDS)
        return false;

    if (!roll_chance_f(float(def.chance) * chanceMult))
        return false;

    last = now;
    return true;
}

void ItemTalentsMgr::HandleProcEvent(Player* player, ProcTrigger trigger, Unit* target,
    float chanceMult, uint32 schoolMask, uint32 eventDamage)
{
    if (!IsEnabled() || !_procsLoaded || _inProc || !player || chanceMult <= 0.0f)
        return;

    PlayerPerks* perks = GetPerks(player->GetGUID().GetCounter());
    if (!perks || perks->row5Procs.empty())
        return;

    // ExecuteProc не мутирует row5Procs (экипировка внутри каста не меняется)
    for (Row5Proc const& proc : perks->row5Procs)
    {
        ProcDef const* def = GetProcDef(proc.triggerSpell);
        if (!def || def->trigger != trigger)
            continue;

        // Канал TAKEN_HIT: school 0 = только "любой урон" (OnDamage), иначе
        // маска школ события должна пересекаться с фильтром дефа
        if (trigger == ProcTrigger::TAKEN_HIT)
        {
            if (def->school == 0 ? schoolMask != 0 : !(def->school & schoolMask))
                continue;
        }
        else if (trigger == ProcTrigger::LOW_HP)
        {
            if (!def->hpThreshold
                || !player->HealthBelowPctDamaged(int32(def->hpThreshold), eventDamage))
                continue;
        }
        else if (trigger == ProcTrigger::BIG_HIT)
        {
            if (!def->hpThreshold
                || uint64(eventDamage) * 100 < uint64(player->GetMaxHealth()) * def->hpThreshold)
                continue;
        }

        if (!RollRow5Proc(*perks, proc.triggerSpell, *def, chanceMult))
            continue;

        ExecuteProc(player, target, proc, *def);
    }
}

void ItemTalentsMgr::ExecuteProc(Player* player, Unit* target, Row5Proc const& proc,
    ProcDef const& def)
{
    // Без миграции спеллов эффекты неисполнимы (LoadDefinitions предупредил)
    if (!sSpellMgr->GetSpellInfo(def.visibleSpell))
        return;

    // Наши касты не порождают новые проки (диспетчеры проверяют _inProc)
    _inProc = true;

    int32 bp = proc.value;
    bool const hasValue = def.coef > 0.0f && bp > 0;
    int32* bpArg = hasValue ? &bp : nullptr;

    switch (def.effect)
    {
        case ProcEffect::DAMAGE:
            if (target)
                player->CastCustomSpell(target, def.visibleSpell, bpArg, nullptr, nullptr, true);
            break;
        case ProcEffect::DOT:
        {
            if (!target)
                break;
            // Периодика тикает каждые 2 сек (dbc): bp = значение на тик
            int32 const ticks = def.durationSecs
                ? std::max<int32>(1, int32(def.durationSecs / 2)) : 3;
            int32 bpTick = std::max(1, bp / ticks);
            player->CastCustomSpell(target, def.visibleSpell, &bpTick, nullptr, nullptr, true);
            break;
        }
        case ProcEffect::HEAL:
        case ProcEffect::ABSORB:
        case ProcEffect::ENERGIZE:
        case ProcEffect::BUFF_SELF:
            // bp и на второй эффект: двухэффектные баффы (СЗ = урон+лечение,
            // Дуэлянт = крит+парирование, АП = ближний+дальний) делят значение
            player->CastCustomSpell(player, def.visibleSpell, bpArg, bpArg, nullptr, true);
            break;
        case ProcEffect::EXTRA_ATTACK:
            // SPELL_EFFECT_ADD_EXTRA_ATTACKS, число ударов - basepoints из dbc
            player->CastSpell(player, def.visibleSpell, true);
            break;
        case ProcEffect::DEBUFF_TARGET:
        {
            if (!target)
                break;
            // Значимые дебаффы (снижение брони) - отрицательный basepoint;
            // фиксированные (замедление/скорость атаки) - bp из dbc
            int32 negative = -bp;
            player->CastCustomSpell(target, def.visibleSpell, hasValue ? &negative : nullptr,
                nullptr, nullptr, true);
            break;
        }
        case ProcEffect::INTERRUPT:
        case ProcEffect::STUN:
            if (target)
                player->CastSpell(target, def.visibleSpell, true);
            break;
        case ProcEffect::AOE_DAMAGE:
            // Землетрясение: все враги в 5 м от игрока (до 8 целей)
            for (Unit* enemy : CollectPveTargets(player, player, 5.0f, 8, nullptr))
                player->CastCustomSpell(enemy, def.visibleSpell, bpArg, nullptr, nullptr, true);
            break;
        case ProcEffect::CLEAVE:
            // Размах/Картечь: цель + до 2 врагов рядом с ней
            if (target)
            {
                player->CastCustomSpell(target, def.visibleSpell, bpArg, nullptr, nullptr, true);
                for (Unit* enemy : CollectPveTargets(player, target, 8.0f, 2, target))
                    player->CastCustomSpell(enemy, def.visibleSpell, bpArg, nullptr, nullptr,
                        true);
            }
            break;
        case ProcEffect::SUMMON:
            SummonPhantom(player, target, proc, def);
            break;
        case ProcEffect::NEXT_HIT_BONUS:
        {
            // Танец клинка: бонус хранится в perks, бафф 108066 - индикатор;
            // потребление - OnProcMeleeDone
            PlayerPerks& perks = EnsurePerks(player);
            perks.nextHitBonus = bp;
            player->CastSpell(player, def.visibleSpell, true);
            break;
        }
        default:
            break;
    }

    _inProc = false;

    // Голос пробуждённого предмета (внутри: полный набор рядов + звук
    // настроен для entry + троттлинг)
    if (Item* item = player->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(proc.itemGuid)))
        PlayItemSound(player, item);
}

// ---------------------------------------------------------------------------
// Фамильяр-фантом (SUMMON, best effort)
// ---------------------------------------------------------------------------

void ItemTalentsMgr::SummonPhantom(Player* player, Unit* target, Row5Proc const& proc,
    ProcDef const& def)
{
    uint32 const durationMs =
        (def.durationSecs ? def.durationSecs : 15) * IN_MILLISECONDS;

    TempSummon* phantom = player->SummonCreature(_phantomCreature, *player,
        TEMPSUMMON_TIMED_DESPAWN, durationMs);
    if (!phantom)
    {
        // TODO(phantom): существо ItemTalents.PhantomCreature отсутствует в
        // creature_template - фантом деградирует до чистого баффа-индикатора
        LOG_WARN("module", "mod-item-talents: failed to summon phantom creature {} - "
            "check ItemTalents.PhantomCreature.", _phantomCreature);
        player->CastSpell(player, def.visibleSpell, true);
        return;
    }

    // Фамильяры гачи - мирные компаньоны (PassiveAI): бьём не через их AI,
    // а прямыми triggered-кастами из UpdatePhantoms
    phantom->SetOwnerGUID(player->GetGUID());
    phantom->SetFaction(player->GetFaction());
    phantom->SetLevel(player->GetLevel());
    phantom->SetReactState(REACT_PASSIVE);

    // Видимый бафф-индикатор (108091) на владельце
    player->CastSpell(player, def.visibleSpell, true);

    ItemTalents::Phantom entry;
    entry.owner = player->GetGUID();
    entry.creature = phantom->GetGUID();
    entry.target = target ? target->GetGUID() : ObjectGuid::Empty;
    entry.value = proc.value;
    entry.remainingMs = int32(durationMs);
    entry.tickMs = 3000;
    _phantoms.push_back(entry);
}

void ItemTalentsMgr::UpdatePhantoms(uint32 diff)
{
    if (_phantoms.empty())
        return;

    for (auto itr = _phantoms.begin(); itr != _phantoms.end();)
    {
        ItemTalents::Phantom& phantom = *itr;
        phantom.remainingMs -= int32(diff);

        Player* owner = ObjectAccessor::FindPlayer(phantom.owner);
        Creature* creature = owner ? ObjectAccessor::GetCreature(*owner, phantom.creature)
            : nullptr;
        if (phantom.remainingMs <= 0 || !owner || !creature || !creature->IsAlive())
        {
            itr = _phantoms.erase(itr);
            continue;
        }

        phantom.tickMs -= int32(diff);
        if (phantom.tickMs <= 0)
        {
            phantom.tickMs += 3000; // болт каждые 3 сек (PERKS: 0.5 x ilvl)

            Unit* target = ObjectAccessor::GetUnit(*owner, phantom.target);
            // Цель умерла/пропала - перехватываем текущую цель владельца
            if (!target || !target->IsAlive() || !IsPveUnit(target))
            {
                target = owner->GetVictim();
                phantom.target = (target && IsPveUnit(target)) ? target->GetGUID()
                    : ObjectGuid::Empty;
            }

            if (target && target->IsAlive() && IsPveUnit(target)
                && creature->IsWithinDistInMap(target, 60.0f))
            {
                // Болт духа: generic NPC Lightning Bolt с bp-override
                // (готовый визуал; свой спелл болта - вне канона CUSTOM_SPELLS)
                int32 bp = phantom.value;
                creature->CastCustomSpell(target, ItemTalents::SPELL_GENERIC_LIGHTNING_BOLT,
                    &bp, nullptr, nullptr, true);
            }
        }

        ++itr;
    }
}
