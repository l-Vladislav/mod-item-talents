/*
 * mod-item-talents: скрипты (WorldScript, PlayerScript, CommandScript).
 *
 * Протокол аддона (ADDON_UI.md §4), ответы SYSTEM-сообщениями с префиксом ITALENT:
 *   .itemtalent info <bag> <slot> | info inv <slot>
 *       ITALENT:HDR:<guid>:<ilvl>:<quality>:<pool>:<rowsOpen>:<nearMaster>:<kills>:<freePts>:<nextNeed>
 *       ITALENT:ROW:<row>:<chosen 0..3>
 *       ITALENT:OPT:<row>:<choice>:<effect>:<value>:<name_ru>
 *       ITALENT:END
 *   .itemtalent choose <itemGuidLow> <row> <choice>  -> ITALENT:OK + свежий info | ITALENT:ERR:<код>
 *   .itemtalent reset  <itemGuidLow> <row>           -> ITALENT:OK + свежий info | ITALENT:ERR:<код>
 *   .itemtalent list -> ITALENT:ITEM:<guid>:<pool>:<quality>:<kills>:<free>:<имя> ... ITALENT:END
 *
 * Координаты info - клиентские: bag 0 (рюкзак, слоты 1..16), bag 1..4 (слоты 1..N);
 * inv <slot> 1..19 (клиентский слот куклы = серверный + 1).
 */

#include "Chat.h"
#include "CommandScript.h"
#include "Creature.h"
#include "Item.h"
#include "ItemTalentsMgr.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "StringConvert.h"

using namespace Acore::ChatCommands;

// ---------------------------------------------------------------------------
// WorldScript: загрузка определений строго в OnStartup (не OnAfterConfigLoad -
// БД на первом OnAfterConfigLoad ещё недоступна, известная грабля проекта).
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
};

// ---------------------------------------------------------------------------
// PlayerScript: опыт предмета + кэш + применение статов через новый хук
// OnPlayerApplyItemMods (конец Player::_ApplyItemMods, симметрично на
// equip/unequip/login/починку).
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
    }
};

// ---------------------------------------------------------------------------
// CommandScript: .itemtalent (SEC_PLAYER, только из игры)
// ---------------------------------------------------------------------------
class itemtalent_commandscript : public CommandScript
{
public:
    itemtalent_commandscript() : CommandScript("itemtalent_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable itemTalentTable =
        {
            { "info",   HandleInfoCommand,   SEC_PLAYER, Console::No },
            { "choose", HandleChooseCommand, SEC_PLAYER, Console::No },
            { "reset",  HandleResetCommand,  SEC_PLAYER, Console::No },
            { "list",   HandleListCommand,   SEC_PLAYER, Console::No },
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

    // Полный info-блок предмета (HDR/ROW/OPT/END)
    static void SendItemInfo(ChatHandler* handler, Player* player, Item* item)
    {
        ItemTemplate const* proto = item->GetTemplate();
        std::optional<char> pool = ItemTalentsMgr::GetPool(proto->Class, proto->SubClass);
        ItemTalents::ItemState& state = sItemTalentsMgr->EnsureState(player, item);

        uint8 const rowsOpen = ItemTalentsMgr::RowsOpenForQuality(proto->Quality);
        uint8 const nearMaster = sItemTalentsMgr->IsNearMaster(player) ? 1 : 0;

        handler->PSendSysMessage("ITALENT:HDR:{}:{}:{}:{}:{}:{}:{}:{}:{}",
            item->GetGUID().GetCounter(), proto->ItemLevel, proto->Quality, *pool, rowsOpen,
            nearMaster, state.kills, sItemTalentsMgr->FreePoints(state),
            sItemTalentsMgr->NextPointNeed(state.kills));

        for (uint8 row = 1; row <= ItemTalents::MAX_ROWS; ++row)
        {
            handler->PSendSysMessage("ITALENT:ROW:{}:{}", row, state.rows[row - 1]);
            for (uint8 choice = 1; choice <= ItemTalents::MAX_CHOICES; ++choice)
                if (ItemTalents::TalentDef const* def = sItemTalentsMgr->GetDef(*pool, row, choice))
                    handler->PSendSysMessage("ITALENT:OPT:{}:{}:{}:{}:{}", row, choice,
                        def->effect, ItemTalentsMgr::CalcValue(*def, proto->ItemLevel),
                        def->nameRu);
        }

        handler->PSendSysMessage("ITALENT:END");
    }

    // Общая валидация для choose/reset: предмет по GUID, надет, в системе, цел.
    // Возвращает nullptr и шлёт ERR, если что-то не так.
    static Item* GetValidatedItem(ChatHandler* handler, Player* player, uint32 itemGuidLow)
    {
        Item* item = player->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(itemGuidLow));
        if (!item)
        {
            SendError(handler, "NO_ITEM");
            return nullptr;
        }

        if (!item->IsEquipped())
        {
            SendError(handler, "NOT_EQUIPPED");
            return nullptr;
        }

        if (!ItemTalentsMgr::IsEligibleItem(item->GetTemplate()))
        {
            SendError(handler, "NO_POOL");
            return nullptr;
        }

        // На сломанном предмете статы ядром сняты - менять таланты нельзя,
        // иначе разъедутся применённые модификаторы.
        if (item->IsBroken())
        {
            SendError(handler, "BROKEN");
            return nullptr;
        }

        return item;
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

    // choose <itemGuidLow> <row> <choice>
    static bool HandleChooseCommand(ChatHandler* handler, uint32 itemGuidLow, uint8 row,
        uint8 choice)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!sItemTalentsMgr->IsEnabled())
        {
            SendError(handler, "DISABLED");
            return true;
        }

        if (row < 1 || row > ItemTalents::MAX_ROWS || choice < 1
            || choice > ItemTalents::MAX_CHOICES)
        {
            SendError(handler, "BAD_ARGS");
            return true;
        }

        Item* item = GetValidatedItem(handler, player, itemGuidLow);
        if (!item)
            return true;

        ItemTemplate const* proto = item->GetTemplate();
        std::optional<char> pool = ItemTalentsMgr::GetPool(proto->Class, proto->SubClass);

        if (row > ItemTalentsMgr::RowsOpenForQuality(proto->Quality))
        {
            SendError(handler, "ROW_LOCKED"); // ряд не открыт качеством
            return true;
        }

        if (row > sItemTalentsMgr->GetMaxImplementedRow())
        {
            SendError(handler, "ROW_SOON"); // v1: ряды 3-5 показываются, но выбор запрещён
            return true;
        }

        if (!sItemTalentsMgr->GetDef(*pool, row, choice))
        {
            SendError(handler, "BAD_CHOICE");
            return true;
        }

        ItemTalents::ItemState& state = sItemTalentsMgr->EnsureState(player, item);
        if (state.rows[row - 1])
        {
            SendError(handler, "ALREADY_CHOSEN");
            return true;
        }

        if (!sItemTalentsMgr->FreePoints(state))
        {
            SendError(handler, "NO_POINTS");
            return true;
        }

        if (!sItemTalentsMgr->IsNearMaster(player))
        {
            SendError(handler, "NO_MASTER");
            return true;
        }

        sItemTalentsMgr->SaveChoice(player, item, row, choice);
        sItemTalentsMgr->ApplyTalent(player, item, row, choice, true);

        // Первый вложенный талант привязывает предмет (DESIGN §2)
        if (!item->IsSoulBound())
        {
            item->SetBinding(true);
            item->SetState(ITEM_CHANGED, player);
        }

        handler->PSendSysMessage("ITALENT:OK");
        SendItemInfo(handler, player, item);
        return true;
    }

    // reset <itemGuidLow> <row>
    static bool HandleResetCommand(ChatHandler* handler, uint32 itemGuidLow, uint8 row)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!sItemTalentsMgr->IsEnabled())
        {
            SendError(handler, "DISABLED");
            return true;
        }

        if (row < 1 || row > ItemTalents::MAX_ROWS)
        {
            SendError(handler, "BAD_ARGS");
            return true;
        }

        Item* item = GetValidatedItem(handler, player, itemGuidLow);
        if (!item)
            return true;

        ItemTalents::ItemState& state = sItemTalentsMgr->EnsureState(player, item);
        uint8 const oldChoice = state.rows[row - 1];
        if (!oldChoice)
        {
            SendError(handler, "NOT_CHOSEN");
            return true;
        }

        if (!sItemTalentsMgr->IsNearMaster(player))
        {
            SendError(handler, "NO_MASTER");
            return true;
        }

        sItemTalentsMgr->ApplyTalent(player, item, row, oldChoice, false);
        sItemTalentsMgr->ResetChoice(player, item, row);

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
            handler->PSendSysMessage("ITALENT:ITEM:{}:{}:{}:{}:{}:{}",
                item->GetGUID().GetCounter(), *pool, proto->Quality, state.kills,
                sItemTalentsMgr->FreePoints(state), proto->Name1);
        }

        handler->PSendSysMessage("ITALENT:END");
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
    new itemtalent_commandscript();
}
