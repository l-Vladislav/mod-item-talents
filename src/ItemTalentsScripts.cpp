/*
 * mod-item-talents: скрипты (WorldScript, PlayerScript, UnitScript,
 * AllCreatureScript, CommandScript).
 *
 * Протокол аддона (ADDON_UI.md §4), ответы SYSTEM-сообщениями с префиксом ITALENT:
 *   .itemtalent info <bag> <slot> | info inv <slot>
 *       ITALENT:HDR:<guid>:<ilvl>:<quality>:<pool>:<rowsOpen>:<nearMaster>:<kills>:<freePts>:<nextNeed>:<baseEpic>
 *         baseEpic (10-е поле, фаза 2): 1 = ряд 5 доступен предмету (базовый
 *         эпик - Quality 4 и корень GA-цепочки не ниже эпика; именные
 *         предметы тоже 1), 0 = потолок 4 ряда ("недоступен для предмета")
 *       ITALENT:ROW:<row>:<chosen slot 0..3>
 *       ITALENT:OPT:<row>:<slot 1..3>:<effect>:<value>:<perkQuality 0..2>:<name_ru>
 *       ITALENT:END
 *     OPT-строки - по РОЛЛАМ предмета (3 случайных варианта из меню ряда,
 *     роллятся лениво в EnsureState); value уже с множителем качества.
 *   .itemtalent choose <itemGuidLow> <row> <slot>  -> ITALENT:OK + свежий info | ITALENT:ERR:<код>
 *   .itemtalent reset  <itemGuidLow> <row>         -> ITALENT:OK + свежий info | ITALENT:ERR:<код>
 *   .itemtalent list -> ITALENT:ITEM:<guid>:<pool>:<quality>:<kills>:<free>:<имя> ... ITALENT:END
 *
 * GM-команды (SEC_GAMEMASTER, тестирование):
 *   .itemtalent awaken <itemGuidLow>      - все очки + выбрать слот 1 в пустых рядах
 *     (фаза 2: пробуждает и ряд 5 - у именных наборов И generic базовых
 *     эпиков; закрытые качеством/гейтом ряды отсекает TryChoose)
 *   .itemtalent setkills <itemGuidLow> <n> - выставить kills (кэш + БД)
 *   .itemtalent reroll <itemGuidLow>      - сброс выборов и роллов, свежий ролл
 *   .itemtalent sound <soundId>           - проиграть себе звук (подбор SoundEntries)
 *
 * Госсип-фолбэк у мастеров оружия (AllCreatureScript): пункт "Пробудить
 * снаряжение" ДОБАВЛЯЕТСЯ к штатному меню тренера (PrepareGossipMenu +
 * AddGossipItemFor + SendPreparedGossip; выбор штатных пунктов - обучение,
 * товары - не перехватываем: CanCreatureGossipSelect реагирует только на
 * свой sender). Флоу: предмет -> ряды -> 3 ролла -> подтверждение; сброс
 * ряда - из меню предмета. Валидация - те же TryChoose/TryReset, что у
 * команды choose/reset, но без проверки nearMaster (игрок и так у мастера).
 *
 * Координаты info - клиентские: bag 0 (рюкзак, слоты 1..16), bag 1..4 (слоты 1..N);
 * inv <slot> 1..19 (клиентский слот куклы = серверный + 1).
 */

#include "Chat.h"
#include "CommandScript.h"
#include "Creature.h"
#include "GossipDef.h"
#include "Item.h"
#include "ItemTalentsMgr.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "LootMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Util.h"

using namespace Acore::ChatCommands;

namespace
{
    // ---- госсип: кодирование состояния в (sender, action) ----
    // DB-опции меню имеют sender = 0 (PlayerGossip.cpp: AddMenuItem(..., 0,
    // OptionType, ...)), скриптовые - маленькие GOSSIP_SENDER_*; наш sender
    // заведомо уникален.
    constexpr uint32 ITEM_TALENTS_GOSSIP_SENDER = 0xF17A;

    enum ItemTalentsGossipOp : uint32
    {
        OP_ROOT           = 1, // список надетых eligible-предметов
        OP_ITEM           = 2, // ряды предмета (equipSlot)
        OP_ROW            = 3, // 3 ролла ряда (equipSlot, row)
        OP_CONFIRM_CHOOSE = 4, // подтверждение выбора (equipSlot, row, slot)
        OP_DO_CHOOSE      = 5, // выбор (equipSlot, row, slot)
        OP_CONFIRM_RESET  = 6, // подтверждение сброса (equipSlot, row)
        OP_DO_RESET       = 7, // сброс (equipSlot, row)
        OP_CLOSE          = 8,
    };

    uint32 MakeAction(uint32 op, uint32 equipSlot = 0, uint32 row = 0, uint32 slot = 0)
    {
        return op | (equipSlot << 8) | (row << 16) | (slot << 20);
    }

    // Русские тексты: ASCII '-' и ':', без длинных тире (глифы 3.3.5!)
    char const* RowName(uint8 row)
    {
        switch (row)
        {
            case 1: return "Заточка";
            case 2: return "Закалка";
            case 3: return "Гравировка";
            case 4: return "Насыщение";
            case 5: return "Пробуждение";
            default: return "";
        }
    }

    char const* QualityName(uint8 quality)
    {
        switch (quality)
        {
            case 1: return "Отличный";
            case 2: return "Совершенный";
            default: return "Обычный";
        }
    }

    // Коды ошибок TryChoose/TryReset -> русский текст для госсипа/чата
    char const* ErrorText(char const* code)
    {
        std::string_view const c(code);
        if (c == "NO_ITEM")        return "Предмет не найден.";
        if (c == "NOT_EQUIPPED")   return "Предмет должен быть надет.";
        if (c == "NO_POOL")        return "Этот предмет нельзя пробудить.";
        if (c == "BROKEN")         return "Сначала почините предмет.";
        if (c == "ROW_LOCKED")     return "Ряд закрыт: нужно качество предмета выше.";
        if (c == "ROW_SOON")       return "Этот ряд пока недоступен: скоро.";
        if (c == "BAD_CHOICE")     return "Такой вариант недоступен.";
        if (c == "ALREADY_CHOSEN") return "В этом ряду уже выбран перк.";
        if (c == "NO_POINTS")      return "Нужен уровень пробуждения выше: больше убийств этим предметом.";
        if (c == "NO_MASTER")      return "Рядом нет мастера оружия.";
        if (c == "NOT_CHOSEN")     return "В этом ряду ничего не выбрано.";
        return "Действие недоступно.";
    }

    // "{N}" в desc_ru -> значение с учётом качества ролла;
    // "{chance}" (именные проки) -> процент срабатывания
    std::string FormatDesc(std::string desc, int32 value, int32 chance = -1)
    {
        std::string::size_type pos = desc.find("{N}");
        if (pos != std::string::npos)
            desc.replace(pos, 3, std::to_string(value));

        if (chance >= 0)
        {
            pos = desc.find("{chance}");
            if (pos != std::string::npos)
                desc.replace(pos, 8, std::to_string(chance));
        }
        return desc;
    }

    // Описание перка для госсипа: ряд 5 подставляет и {chance} - у generic-
    // проков из item_talent_procs (base дефа = триггер-спелл), у именных
    // перков из item_talent_named.proc_chance
    std::string PerkDesc(ItemTemplate const* proto, uint8 row,
        ItemTalents::TalentDef const* def, uint8 choice, int32 value)
    {
        int32 chance = -1;
        if (row == ItemTalents::MAX_ROWS)
        {
            if (def->effect == "PROC")
                chance = sItemTalentsMgr->GetProcChance(uint32(def->base));
            else if (ItemTalents::NamedDef const* namedDef =
                sItemTalentsMgr->GetNamedDef(proto->ItemId, choice))
                chance = int32(namedDef->procChance);
        }

        return FormatDesc(def->descRu, value, chance);
    }
}

// ---------------------------------------------------------------------------
// WorldScript: загрузка определений строго в OnStartup (не OnAfterConfigLoad -
// БД на первом OnAfterConfigLoad ещё недоступна, известная грабля проекта).
// OnUpdate: периодическое обновление кэша spawnId немезид (перк ряда 4).
// ---------------------------------------------------------------------------
class ItemTalents_World : public WorldScript
{
public:
    ItemTalents_World() : WorldScript("ItemTalents_World") { }

    void OnStartup() override
    {
        sItemTalentsMgr->LoadConfig();
        sItemTalentsMgr->LoadDefinitions();
    }

    void OnAfterConfigLoad(bool reload) override
    {
        if (reload)
            sItemTalentsMgr->LoadConfig();
    }

    void OnUpdate(uint32 diff) override
    {
        sItemTalentsMgr->UpdateNemesisCache(diff);
        // Болты активных Фамильяров-фантомов (прок ряда 5, пул H)
        sItemTalentsMgr->UpdatePhantoms(diff);
    }
};

// ---------------------------------------------------------------------------
// PlayerScript: опыт предмета + кэш + применение статов через хук
// OnPlayerApplyItemMods (конец Player::_ApplyItemMods, симметрично на
// equip/unequip/login/починку) + хуки эффектов рядов 3-4.
// ---------------------------------------------------------------------------
class ItemTalents_Player : public PlayerScript
{
public:
    ItemTalents_Player() : PlayerScript("ItemTalents_Player") { }

    void OnPlayerLogin(Player* player) override
    {
        if (!sItemTalentsMgr->IsEnabled() || sItemTalentsMgr->ShouldIgnorePlayer(player))
            return;

        sItemTalentsMgr->LoadPlayerState(player);
    }

    void OnPlayerCreatureKill(Player* killer, Creature* killed) override
    {
        if (!sItemTalentsMgr->IsEnabled() || !killer || !killed)
            return;

        // Плейерботы не копят опыт предметов (ItemTalents.IgnoreBots)
        if (sItemTalentsMgr->ShouldIgnorePlayer(killer))
            return;

        // Только убийства, приносящие честь/опыт: не серые, не криттеры,
        // не питомцы/тотемы (PvP-убийства сюда не попадают вовсе).
        if (!killer->isHonorOrXPTarget(killed))
            return;

        sItemTalentsMgr->AddKill(killer);
        // Именной прок JORDAN_COIN (шанс/ICD внутри)
        sItemTalentsMgr->OnKillProcs(killer);
        // v1: пробуждённое оружие в основной руке иногда "подаёт голос"
        sItemTalentsMgr->OnKillSoundChance(killer);
    }

    void OnPlayerSave(Player* player) override
    {
        sItemTalentsMgr->FlushKills(player);
    }

    void OnPlayerLogout(Player* player) override
    {
        sItemTalentsMgr->FlushKills(player);
        sItemTalentsMgr->UnloadPlayerState(player);
    }

    void OnPlayerApplyItemMods(Player* player, Item* item, uint8 slot, bool apply) override
    {
        if (slot >= EQUIPMENT_SLOT_END)
            return;

        sItemTalentsMgr->ApplyAllTalents(player, item, apply);

        // Именной звук полностью пробуждённого оружия при надевании
        // (внутри: только слот основной руки, только в мире, троттлинг)
        if (apply)
            sItemTalentsMgr->PlayItemSound(player, item);
    }

    // DURA_SAVE: N% событий износа предмета с перком пропускается
    void OnPlayerDurabilityPointsLoss(Player* player, Item* item, int32& points) override
    {
        sItemTalentsMgr->HandleDurabilityLoss(player, item, points);
    }

    // GOLD_XP_PCT (золото): бонус к деньгам с добычи СУЩЕСТВ. Хук идёт до
    // раздачи денег: в группе увеличенный котёл делится на всех - осознанно
    // (бонус мал, точность на группу не оправдывает второй хук).
    void OnPlayerBeforeLootMoney(Player* player, Loot* loot) override
    {
        if (!loot || !loot->gold)
            return;

        if (!loot->sourceWorldObjectGUID.IsCreatureOrVehicle())
            return; // сундуки/ГО/прочее - перк только "с существ"

        if (uint32 const pct = sItemTalentsMgr->GetGoldBonusPct(player))
            loot->gold += CalculatePct(loot->gold, pct);
    }

    // GOLD_XP_PCT (опыт): +1% за предмет, все источники опыта
    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* /*victim*/,
        uint8 /*xpSource*/) override
    {
        if (!amount)
            return;

        if (uint32 const pct = sItemTalentsMgr->GetXpBonusPct(player))
            amount += CalculatePct(amount, pct);
    }

    // Именной прок JORDAN_LIGHTNING: успешный боевой каст по врагу.
    // Хук зовётся из Spell::cast (после проверок, перед исполнением).
    void OnPlayerSpellCast(Player* player, Spell* spell, bool /*skipCheck*/) override
    {
        if (!player || !spell)
            return;

        SpellInfo const* spellInfo = spell->GetSpellInfo();
        if (!spellInfo || spellInfo->IsPositive())
            return;

        Unit* target = spell->m_targets.GetUnitTarget();
        if (!target || target == player || !player->IsValidAttackTarget(target))
            return;

        sItemTalentsMgr->OnCombatSpellCast(player, target, spellInfo->Id);
    }
};

// ---------------------------------------------------------------------------
// UnitScript: бонус урона по немезидам (NEMESIS_DMG_PCT) + пересчёт
// "Уз фамильяра" при изменении владельческих аур (FAMILIAR_ALL_STATS) +
// события проков ряда 5 (фаза 2):
//  - ModifyMeleeDamage: замах игрока (MELEE_HIT/крит-аппроксимация, Танец
//    клинка) и входящий замах (TAKEN_HIT физ., DODGE/PARRY/BLOCK/TAKEN_CRIT);
//  - ModifySpellDamageTaken: спелл-урон игрока (мили-абилки/выстрелы/
//    заклинания по DmgClass) и полученный спелл-урон (TAKEN_HIT по школе);
//  - OnDamage (Unit::DealDamage, после митигации): канал "получен урон"
//    (Мановая пелена), LOW_HP, BIG_HIT.
// Включаем только нужные хуки - UnitScript-хуки очень горячие.
// ---------------------------------------------------------------------------
class ItemTalents_Unit : public UnitScript
{
public:
    ItemTalents_Unit() : UnitScript("ItemTalents_Unit", true,
        {
            UNITHOOK_MODIFY_MELEE_DAMAGE,
            UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN,
            UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK,
            UNITHOOK_ON_DAMAGE,
            UNITHOOK_ON_AURA_APPLY,
            UNITHOOK_ON_AURA_REMOVE,
        }) { }

    void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage) override
    {
        AddNemesisBonus(target, attacker, damage);

        // Проки ряда 5. ВНИМАНИЕ: хук зовётся ДО броска исхода замаха
        // (Unit::CalculateMeleeDamage) - модель аппроксимаций описана в
        // ItemTalentsProcs.cpp.
        if (attacker && attacker->IsPlayer())
            sItemTalentsMgr->OnProcMeleeDone(attacker->ToPlayer(), target, damage);
        else if (target && target->IsPlayer())
            sItemTalentsMgr->OnProcMeleeTaken(target->ToPlayer(), attacker);
    }

    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage,
        SpellInfo const* spellInfo) override
    {
        if (damage <= 0)
            return;

        uint32 udamage = uint32(damage);
        AddNemesisBonus(target, attacker, udamage);
        damage = int32(udamage);

        // Проки ряда 5 (урон уже посчитан, значение не меняем)
        if (attacker && attacker->IsPlayer())
            sItemTalentsMgr->OnProcSpellDone(attacker->ToPlayer(), target, spellInfo);
        else if (target && target->IsPlayer())
            sItemTalentsMgr->OnProcSpellTaken(target->ToPlayer(), attacker, spellInfo);
    }

    // Любой фактический урон (после митигации, до списания здоровья):
    // TAKEN_HIT "любой урон" + LOW_HP + BIG_HIT
    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        if (victim && victim->IsPlayer())
            sItemTalentsMgr->OnProcAnyDamageTaken(victim->ToPlayer(), attacker, damage);
    }

    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage,
        SpellInfo const* /*spellInfo*/) override
    {
        AddNemesisBonus(target, attacker, damage);
    }

    void OnAuraApply(Unit* unit, Aura* aura) override
    {
        if (!unit || !aura || !unit->IsPlayer())
            return;

        if (sItemTalentsMgr->IsFamiliarOwnerAura(aura->GetId()))
            sItemTalentsMgr->OnFamiliarAuraChanged(unit->ToPlayer());
    }

    void OnAuraRemove(Unit* unit, AuraApplication* aurApp, AuraRemoveMode /*mode*/) override
    {
        if (!unit || !aurApp || !unit->IsPlayer())
            return;

        // Хук идёт ПОСЛЕ удаления из m_appliedAuras (Unit.cpp ~4894) -
        // пересчёт увидит актуальное отсутствие ауры.
        if (sItemTalentsMgr->IsFamiliarOwnerAura(aurApp->GetBase()->GetId()))
            sItemTalentsMgr->OnFamiliarAuraChanged(unit->ToPlayer());
    }

private:
    static void AddNemesisBonus(Unit* target, Unit* attacker, uint32& damage)
    {
        if (!damage || !attacker || !target)
            return;

        Player* player = attacker->ToPlayer();
        if (!player)
            return;

        uint32 const pct = sItemTalentsMgr->GetNemesisBonusPct(player);
        if (!pct)
            return;

        Creature* creature = target->ToCreature();
        if (!creature || !sItemTalentsMgr->IsNemesisTarget(creature))
            return;

        damage += CalculatePct(damage, pct);
    }
};

// ---------------------------------------------------------------------------
// AllCreatureScript: госсип-фолбэк "Пробудить снаряжение" у мастеров оружия.
// ---------------------------------------------------------------------------
class ItemTalents_Creature : public AllCreatureScript
{
public:
    ItemTalents_Creature() : AllCreatureScript("ItemTalents_Creature") { }

    bool CanCreatureGossipHello(Player* player, Creature* creature) override
    {
        if (!sItemTalentsMgr->IsEnabled() || !sItemTalentsMgr->IsMasterEntry(creature->GetEntry()))
            return false;

        // Штатное меню НПЦ (обучение навыкам, товары, квесты) + наш пункт.
        // Зеркалит дефолтную ветку WorldSession::HandleGossipHelloOpcode,
        // поэтому обучение у тренера продолжает работать как раньше.
        player->PrepareGossipMenu(creature, creature->GetGossipMenuId(), true);
        AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1, "Пробудить снаряжение",
            ITEM_TALENTS_GOSSIP_SENDER, MakeAction(OP_ROOT));
        player->SendPreparedGossip(creature);
        return true; // hello обработан (меню уже отправлено)
    }

    bool CanCreatureGossipSelect(Player* player, Creature* creature, uint32 sender,
        uint32 action) override
    {
        // Чужие пункты (в т.ч. штатные "обучение"/"товары", sender = 0) не
        // трогаем - ядро обработает их обычным путём.
        if (sender != ITEM_TALENTS_GOSSIP_SENDER)
            return false;

        if (!sItemTalentsMgr->IsEnabled() || !sItemTalentsMgr->IsMasterEntry(creature->GetEntry()))
        {
            CloseGossipMenuFor(player);
            return true;
        }

        uint32 const op = action & 0xFF;
        uint8 const equipSlot = uint8((action >> 8) & 0xFF);
        uint8 const row = uint8((action >> 16) & 0xF);
        uint8 const slot = uint8((action >> 20) & 0xF);

        switch (op)
        {
            case OP_ROOT:
                ShowRoot(player, creature);
                break;
            case OP_ITEM:
                ShowItem(player, creature, equipSlot);
                break;
            case OP_ROW:
                ShowRow(player, creature, equipSlot, row);
                break;
            case OP_CONFIRM_CHOOSE:
                ShowConfirmChoose(player, creature, equipSlot, row, slot);
                break;
            case OP_DO_CHOOSE:
            {
                Item* item = GetEquippedItem(player, equipSlot);
                char const* error = sItemTalentsMgr->TryChoose(player, item, row, slot,
                    /*requireNearMaster=*/false); // игрок и так у мастера
                ChatHandler handler(player->GetSession());
                if (error)
                    handler.PSendSysMessage("|cffff0000{}|r", ErrorText(error));
                else
                    handler.PSendSysMessage("|cff00ff00Перк вложен. Предмет откликается на прикосновение.|r");
                ShowItem(player, creature, equipSlot);
                break;
            }
            case OP_CONFIRM_RESET:
                ShowConfirmReset(player, creature, equipSlot, row);
                break;
            case OP_DO_RESET:
            {
                Item* item = GetEquippedItem(player, equipSlot);
                char const* error = sItemTalentsMgr->TryReset(player, item, row,
                    /*requireNearMaster=*/false);
                ChatHandler handler(player->GetSession());
                if (error)
                    handler.PSendSysMessage("|cffff0000{}|r", ErrorText(error));
                else
                    handler.PSendSysMessage("|cff00ff00Ряд сброшен, очко возвращено.|r");
                ShowItem(player, creature, equipSlot);
                break;
            }
            case OP_CLOSE:
            default:
                CloseGossipMenuFor(player);
                break;
        }

        return true;
    }

private:
    static Item* GetEquippedItem(Player* player, uint8 equipSlot)
    {
        if (equipSlot >= EQUIPMENT_SLOT_END)
            return nullptr;

        return player->GetItemByPos(INVENTORY_SLOT_BAG_0, equipSlot);
    }

    // Список надетых eligible-предметов: имя + kills + свободные очки
    static void ShowRoot(Player* player, Creature* creature)
    {
        ClearGossipMenuFor(player);

        uint8 shown = 0;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item || !ItemTalentsMgr::IsEligibleItem(item->GetTemplate()))
                continue;

            ItemTalents::ItemState& state = sItemTalentsMgr->EnsureState(player, item);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                Acore::StringFormat("{} - убийств: {}, уровень пробуждения: {}",
                    item->GetTemplate()->Name1, state.kills,
                    sItemTalentsMgr->EarnedPoints(state.kills)),
                ITEM_TALENTS_GOSSIP_SENDER, MakeAction(OP_ITEM, slot));
            ++shown;
        }

        if (!shown)
            AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                "Нет подходящих надетых предметов.",
                ITEM_TALENTS_GOSSIP_SENDER, MakeAction(OP_CLOSE));

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "До встречи.",
            ITEM_TALENTS_GOSSIP_SENDER, MakeAction(OP_CLOSE));
        SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature);
    }

    // Ряды предмета: выбрать перк / сбросить / статус
    static void ShowItem(Player* player, Creature* creature, uint8 equipSlot)
    {
        Item* item = GetEquippedItem(player, equipSlot);
        if (char const* error = sItemTalentsMgr->ValidateUsableItem(player, item))
        {
            ChatHandler(player->GetSession()).PSendSysMessage("|cffff0000{}|r", ErrorText(error));
            ShowRoot(player, creature);
            return;
        }

        ItemTemplate const* proto = item->GetTemplate();
        ItemTalents::ItemState& state = sItemTalentsMgr->EnsureState(player, item);
        uint8 const rowsOpen = sItemTalentsMgr->RowsOpenForItem(proto);

        ClearGossipMenuFor(player);

        // Шапка-строка (клик просто обновляет меню)
        uint32 const nextNeed = sItemTalentsMgr->NextPointNeed(state.kills);
        std::string header = Acore::StringFormat("{}: убийств {}, уровень пробуждения {} из 5",
            proto->Name1, state.kills, sItemTalentsMgr->EarnedPoints(state.kills));
        if (nextNeed)
            header += Acore::StringFormat(" (след. уровень: {} убийств)", nextNeed);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, header,
            ITEM_TALENTS_GOSSIP_SENDER, MakeAction(OP_ITEM, equipSlot));

        for (uint8 row = 1; row <= ItemTalents::MAX_ROWS; ++row)
        {
            if (uint8 const chosenSlot = state.rows[row - 1])
            {
                // Выбранный перк -> предложение сброса
                std::string perkName = "?";
                ItemTalents::RollSlot const& roll = state.rolls[row - 1][chosenSlot - 1];
                if (roll.choice)
                    if (ItemTalents::TalentDef const* def =
                        sItemTalentsMgr->GetDefForItem(proto, row, roll.choice))
                        perkName = def->nameRu;

                AddGossipItemFor(player, GOSSIP_ICON_BATTLE,
                    Acore::StringFormat("Ряд {} ({}): [{}] - сбросить", row, RowName(row),
                        perkName),
                    ITEM_TALENTS_GOSSIP_SENDER, MakeAction(OP_CONFIRM_RESET, equipSlot, row));
            }
            else if (row > rowsOpen)
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                    Acore::StringFormat("Ряд {} ({}): закрыто, нужно качество выше", row,
                        RowName(row)),
                    ITEM_TALENTS_GOSSIP_SENDER, MakeAction(OP_ITEM, equipSlot));
            else if (!sItemTalentsMgr->IsRowSelectable(proto, row))
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                    Acore::StringFormat("Ряд {} ({}): скоро", row, RowName(row)),
                    ITEM_TALENTS_GOSSIP_SENDER, MakeAction(OP_ITEM, equipSlot));
            else if (sItemTalentsMgr->EarnedPoints(state.kills) < row)
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                    Acore::StringFormat("Ряд {} ({}): нужен уровень пробуждения {} ({} убийств)",
                        row, RowName(row), row, sItemTalentsMgr->GetRowThreshold(row)),
                    ITEM_TALENTS_GOSSIP_SENDER, MakeAction(OP_ITEM, equipSlot));
            else
                AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1,
                    Acore::StringFormat("Ряд {} ({}): выбрать перк", row, RowName(row)),
                    ITEM_TALENTS_GOSSIP_SENDER, MakeAction(OP_ROW, equipSlot, row));
        }

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "<- Назад к списку предметов",
            ITEM_TALENTS_GOSSIP_SENDER, MakeAction(OP_ROOT));
        SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature);
    }

    // 3 ролла ряда: имя + значение + качество
    static void ShowRow(Player* player, Creature* creature, uint8 equipSlot, uint8 row)
    {
        Item* item = GetEquippedItem(player, equipSlot);
        if (!item || row < 1 || row > ItemTalents::MAX_ROWS
            || sItemTalentsMgr->ValidateUsableItem(player, item))
        {
            ShowRoot(player, creature);
            return;
        }

        ItemTemplate const* proto = item->GetTemplate();
        ItemTalents::ItemState& state = sItemTalentsMgr->EnsureState(player, item);

        if (state.rows[row - 1]) // уже выбран - назад к предмету
        {
            ShowItem(player, creature, equipSlot);
            return;
        }

        ClearGossipMenuFor(player);

        if (sItemTalentsMgr->EarnedPoints(state.kills) < row)
            AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                Acore::StringFormat(
                    "Нужен уровень пробуждения {} ({} убийств этим предметом).",
                    row, sItemTalentsMgr->GetRowThreshold(row)),
                ITEM_TALENTS_GOSSIP_SENDER, MakeAction(OP_ITEM, equipSlot));

        // Ряд 5 (именной И generic-проки) роллится без качества - "Обычный"
        // в подписи не пишем
        bool const named = row == ItemTalents::MAX_ROWS;
        for (uint8 slot = 1; slot <= ItemTalents::MAX_SLOTS; ++slot)
        {
            ItemTalents::RollSlot const& roll = state.rolls[row - 1][slot - 1];
            if (!roll.choice)
                continue;

            ItemTalents::TalentDef const* def =
                sItemTalentsMgr->GetDefForItem(proto, row, roll.choice);
            if (!def)
                continue;

            int32 const value = sItemTalentsMgr->CalcValue(*def, proto->ItemLevel, roll.quality);
            std::string const desc = PerkDesc(proto, row, def, roll.choice, value);
            std::string const text = named
                ? Acore::StringFormat("{} - {}", def->nameRu, desc)
                : Acore::StringFormat("{}: {} - {}", QualityName(roll.quality), def->nameRu,
                    desc);
            AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1, text,
                ITEM_TALENTS_GOSSIP_SENDER, MakeAction(OP_CONFIRM_CHOOSE, equipSlot, row, slot));
        }

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "<- Назад к предмету",
            ITEM_TALENTS_GOSSIP_SENDER, MakeAction(OP_ITEM, equipSlot));
        SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature);
    }

    static void ShowConfirmChoose(Player* player, Creature* creature, uint8 equipSlot,
        uint8 row, uint8 slot)
    {
        Item* item = GetEquippedItem(player, equipSlot);
        if (!item || row < 1 || row > ItemTalents::MAX_ROWS || slot < 1
            || slot > ItemTalents::MAX_SLOTS || sItemTalentsMgr->ValidateUsableItem(player, item))
        {
            ShowRoot(player, creature);
            return;
        }

        ItemTemplate const* proto = item->GetTemplate();
        ItemTalents::ItemState& state = sItemTalentsMgr->EnsureState(player, item);
        ItemTalents::RollSlot const& roll = state.rolls[row - 1][slot - 1];
        ItemTalents::TalentDef const* def =
            roll.choice ? sItemTalentsMgr->GetDefForItem(proto, row, roll.choice) : nullptr;
        if (!def)
        {
            ShowItem(player, creature, equipSlot);
            return;
        }

        ClearGossipMenuFor(player);

        int32 const value = sItemTalentsMgr->CalcValue(*def, proto->ItemLevel, roll.quality);
        std::string text = Acore::StringFormat("Подтвердить: {} - {} (1 очко)", def->nameRu,
            PerkDesc(proto, row, def, roll.choice, value));
        if (!item->IsSoulBound())
            text += ". Внимание: предмет станет персональным!";
        AddGossipItemFor(player, GOSSIP_ICON_BATTLE, text,
            ITEM_TALENTS_GOSSIP_SENDER, MakeAction(OP_DO_CHOOSE, equipSlot, row, slot));

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Отмена",
            ITEM_TALENTS_GOSSIP_SENDER, MakeAction(OP_ROW, equipSlot, row));
        SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature);
    }

    static void ShowConfirmReset(Player* player, Creature* creature, uint8 equipSlot, uint8 row)
    {
        Item* item = GetEquippedItem(player, equipSlot);
        if (!item || row < 1 || row > ItemTalents::MAX_ROWS
            || sItemTalentsMgr->ValidateUsableItem(player, item))
        {
            ShowRoot(player, creature);
            return;
        }

        ClearGossipMenuFor(player);
        AddGossipItemFor(player, GOSSIP_ICON_BATTLE,
            Acore::StringFormat("Подтвердить сброс ряда {} ({}). Очко вернется, убийства не сгорят.",
                row, RowName(row)),
            ITEM_TALENTS_GOSSIP_SENDER, MakeAction(OP_DO_RESET, equipSlot, row));
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Отмена",
            ITEM_TALENTS_GOSSIP_SENDER, MakeAction(OP_ITEM, equipSlot));
        SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature);
    }
};

// ---------------------------------------------------------------------------
// CommandScript: .itemtalent (игрок + GM-подкоманды тестирования)
// ---------------------------------------------------------------------------
class itemtalent_commandscript : public CommandScript
{
public:
    itemtalent_commandscript() : CommandScript("itemtalent_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable itemTalentTable =
        {
            { "info",     HandleInfoCommand,     SEC_PLAYER,     Console::No },
            { "choose",   HandleChooseCommand,   SEC_PLAYER,     Console::No },
            { "reset",    HandleResetCommand,    SEC_PLAYER,     Console::No },
            { "list",     HandleListCommand,     SEC_PLAYER,     Console::No },
            // GM-команды тестирования
            { "awaken",   HandleAwakenCommand,   SEC_GAMEMASTER, Console::No },
            { "setkills", HandleSetKillsCommand, SEC_GAMEMASTER, Console::No },
            { "reroll",   HandleRerollCommand,   SEC_GAMEMASTER, Console::No },
            { "sound",    HandleSoundCommand,    SEC_GAMEMASTER, Console::No },
        };
        static ChatCommandTable commandTable =
        {
            { "itemtalent", itemTalentTable }
        };
        return commandTable;
    }

private:
    static void SendError(ChatHandler* handler, char const* code)
    {
        handler->PSendSysMessage("ITALENT:ERR:{}", code);
    }

    // Полный info-блок предмета (HDR/ROW/OPT/END). Именные перки ряда 5
    // уезжают обычными OPT-строками (quality всегда 0).
    static void SendItemInfo(ChatHandler* handler, Player* player, Item* item)
    {
        ItemTemplate const* proto = item->GetTemplate();
        std::optional<char> pool = ItemTalentsMgr::GetPool(proto->Class, proto->SubClass);
        // EnsureState лениво роллит слоты предмета (EnsureRolled внутри)
        ItemTalents::ItemState& state = sItemTalentsMgr->EnsureState(player, item);

        uint8 const rowsOpen = sItemTalentsMgr->RowsOpenForItem(proto);
        uint8 const nearMaster = sItemTalentsMgr->IsNearMaster(player) ? 1 : 0;
        // 10-е поле baseEpic: ряд 5 доступен предмету (базовый эпик или
        // именной набор - у именных тоже 1); см. шапку файла
        uint8 const baseEpic = (sItemTalentsMgr->IsBaseEpic(proto)
            || sItemTalentsMgr->HasNamedSet(proto->ItemId)) ? 1 : 0;

        handler->PSendSysMessage("ITALENT:HDR:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}",
            item->GetGUID().GetCounter(), proto->ItemLevel, proto->Quality, *pool, rowsOpen,
            nearMaster, state.kills, sItemTalentsMgr->EarnedPoints(state.kills),
            sItemTalentsMgr->NextPointNeed(state.kills), baseEpic);

        for (uint8 row = 1; row <= ItemTalents::MAX_ROWS; ++row)
        {
            handler->PSendSysMessage("ITALENT:ROW:{}:{}", row, state.rows[row - 1]);
            for (uint8 slot = 1; slot <= ItemTalents::MAX_SLOTS; ++slot)
            {
                ItemTalents::RollSlot const& roll = state.rolls[row - 1][slot - 1];
                if (!roll.choice)
                    continue;

                if (ItemTalents::TalentDef const* def =
                    sItemTalentsMgr->GetDefForItem(proto, row, roll.choice))
                    handler->PSendSysMessage("ITALENT:OPT:{}:{}:{}:{}:{}:{}", row, slot,
                        def->effect,
                        sItemTalentsMgr->CalcValue(*def, proto->ItemLevel, roll.quality),
                        roll.quality, def->nameRu);
            }
        }

        handler->PSendSysMessage("ITALENT:END");
    }

    static Item* GetItemByGuidLow(Player* player, uint32 itemGuidLow)
    {
        return player->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(itemGuidLow));
    }

    // info <bag> <slot> | info inv <slot> (клиентские координаты, см. шапку файла)
    static bool HandleInfoCommand(ChatHandler* handler, std::string bagArg, uint8 slot)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!sItemTalentsMgr->IsEnabled())
        {
            SendError(handler, "DISABLED");
            return true;
        }

        Item* item = nullptr;
        if (bagArg == "inv")
        {
            if (slot < 1 || slot > EQUIPMENT_SLOT_END)
            {
                SendError(handler, "BAD_ARGS");
                return true;
            }
            item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot - 1);
        }
        else if (Optional<uint8> bag = Acore::StringTo<uint8>(bagArg))
        {
            if (*bag == 0)
            {
                uint8 const backpackSize = INVENTORY_SLOT_ITEM_END - INVENTORY_SLOT_ITEM_START;
                if (slot < 1 || slot > backpackSize)
                {
                    SendError(handler, "BAD_ARGS");
                    return true;
                }
                item = player->GetItemByPos(INVENTORY_SLOT_BAG_0,
                    INVENTORY_SLOT_ITEM_START + slot - 1);
            }
            else if (*bag <= 4 && slot >= 1)
                item = player->GetItemByPos(INVENTORY_SLOT_BAG_START + *bag - 1, slot - 1);
            else
            {
                SendError(handler, "BAD_ARGS");
                return true;
            }
        }
        else
        {
            SendError(handler, "BAD_ARGS");
            return true;
        }

        if (!item)
        {
            SendError(handler, "NO_ITEM");
            return true;
        }

        if (!ItemTalentsMgr::IsEligibleItem(item->GetTemplate()))
        {
            SendError(handler, "NO_POOL");
            return true;
        }

        SendItemInfo(handler, player, item);
        return true;
    }

    // choose <itemGuidLow> <row> <slot>
    static bool HandleChooseCommand(ChatHandler* handler, uint32 itemGuidLow, uint8 row,
        uint8 slot)
    {
        Player* player = handler->GetSession()->GetPlayer();
        Item* item = GetItemByGuidLow(player, itemGuidLow);

        // Вся валидация/запись/привязка - в общем с госсипом хелпере
        if (char const* error = sItemTalentsMgr->TryChoose(player, item, row, slot,
            /*requireNearMaster=*/true))
        {
            SendError(handler, error);
            return true;
        }

        handler->PSendSysMessage("ITALENT:OK");
        SendItemInfo(handler, player, item);
        return true;
    }

    // reset <itemGuidLow> <row>
    static bool HandleResetCommand(ChatHandler* handler, uint32 itemGuidLow, uint8 row)
    {
        Player* player = handler->GetSession()->GetPlayer();
        Item* item = GetItemByGuidLow(player, itemGuidLow);

        if (char const* error = sItemTalentsMgr->TryReset(player, item, row,
            /*requireNearMaster=*/true))
        {
            SendError(handler, error);
            return true;
        }

        handler->PSendSysMessage("ITALENT:OK");
        SendItemInfo(handler, player, item);
        return true;
    }

    // list: все надетые предметы системы
    static bool HandleListCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!sItemTalentsMgr->IsEnabled())
        {
            SendError(handler, "DISABLED");
            return true;
        }

        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;

            ItemTemplate const* proto = item->GetTemplate();
            if (!ItemTalentsMgr::IsEligibleItem(proto))
                continue;

            std::optional<char> pool = ItemTalentsMgr::GetPool(proto->Class, proto->SubClass);
            ItemTalents::ItemState& state = sItemTalentsMgr->EnsureState(player, item);
            // slot+1 = клиентский inv-слот; spent нужен аддону для строки
            // "Пробуждён" в тултипе предмета
            handler->PSendSysMessage("ITALENT:ITEM:{}:{}:{}:{}:{}:{}:{}:{}",
                slot + 1, item->GetGUID().GetCounter(), *pool, proto->Quality,
                state.kills, sItemTalentsMgr->EarnedPoints(state.kills),
                ItemTalentsMgr::SpentPoints(state), proto->Name1);
        }

        handler->PSendSysMessage("ITALENT:END");
        return true;
    }

    // ---- GM-команды тестирования ----

    // awaken <itemGuidLow>: все очки + слот 1 в каждом пустом доступном ряду
    static bool HandleAwakenCommand(ChatHandler* handler, uint32 itemGuidLow)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!sItemTalentsMgr->IsEnabled())
        {
            SendError(handler, "DISABLED");
            return true;
        }

        Item* item = GetItemByGuidLow(player, itemGuidLow);
        if (char const* error = sItemTalentsMgr->ValidateUsableItem(player, item))
        {
            SendError(handler, error);
            return true;
        }

        // Максимальный кумулятивный порог = все 5 очков
        sItemTalentsMgr->SetKills(player, item, sItemTalentsMgr->MaxPointsKills());

        // Слот 1 в каждый пустой ряд; TryChoose сам отсечёт закрытые качеством
        // и нереализованные ряды (ROW_LOCKED/ROW_SOON игнорируем)
        for (uint8 row = 1; row <= ItemTalents::MAX_ROWS; ++row)
            sItemTalentsMgr->TryChoose(player, item, row, 1, /*requireNearMaster=*/false);

        handler->PSendSysMessage("ITALENT:OK");
        SendItemInfo(handler, player, item);
        return true;
    }

    // setkills <itemGuidLow> <n>
    static bool HandleSetKillsCommand(ChatHandler* handler, uint32 itemGuidLow, uint32 kills)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!sItemTalentsMgr->IsEnabled())
        {
            SendError(handler, "DISABLED");
            return true;
        }

        Item* item = GetItemByGuidLow(player, itemGuidLow);
        if (char const* error = sItemTalentsMgr->ValidateUsableItem(player, item))
        {
            SendError(handler, error);
            return true;
        }

        sItemTalentsMgr->SetKills(player, item, kills);
        handler->PSendSysMessage("ITALENT:OK");
        SendItemInfo(handler, player, item);
        return true;
    }

    // reroll <itemGuidLow>: сброс выборов + новые роллы
    static bool HandleRerollCommand(ChatHandler* handler, uint32 itemGuidLow)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!sItemTalentsMgr->IsEnabled())
        {
            SendError(handler, "DISABLED");
            return true;
        }

        Item* item = GetItemByGuidLow(player, itemGuidLow);
        if (char const* error = sItemTalentsMgr->ValidateUsableItem(player, item))
        {
            SendError(handler, error);
            return true;
        }

        sItemTalentsMgr->RerollItem(player, item);
        handler->PSendSysMessage("ITALENT:OK");
        SendItemInfo(handler, player, item);
        return true;
    }

    // sound <soundId>: проиграть себе звук (подбор звуков SoundEntries)
    static bool HandleSoundCommand(ChatHandler* handler, uint32 soundId)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!soundId)
        {
            SendError(handler, "BAD_ARGS");
            return true;
        }

        player->PlayDirectSound(soundId, player);
        handler->PSendSysMessage("ITALENT:SOUND:{}", soundId);
        return true;
    }
};

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
void AddSC_item_talents_scripts()
{
    new ItemTalents_World();
    new ItemTalents_Player();
    new ItemTalents_Unit();
    new ItemTalents_Creature();
    new itemtalent_commandscript();
}
