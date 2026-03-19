/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "BuyAction.h"

#include "AuctionHouseMgr.h"
#include "BudgetValues.h"
#include "Event.h"
#include "ItemCountValue.h"
#include "ItemUsageValue.h"
#include "ItemVisitors.h"
#include "Log.h"
#include "Playerbots.h"
#include "StatsWeightCalculator.h"

#include <limits>

bool BuyAction::Execute(Event event)
{
    bool buyUseful = false;
    bool buyAuction = false;
    ItemIds itemIds;
    std::string const link = event.getParam();

    if (link == "vendor")
        buyUseful = true;
    else if (link == "auction")
        buyAuction = true;
    else
    {
        itemIds = chat->parseItems(link);
    }

    if (buyAuction)
    {
        if (!sPlayerbotAIConfig.enableAuctionHouseBotting)
            return false;

        return BuyFromAuctionHouse();
    }

    GuidVector vendors = botAI->GetAiObjectContext()->GetValue<GuidVector>("nearest npcs")->Get();

    bool vendored = false;
    bool result = false;
    for (GuidVector::iterator i = vendors.begin(); i != vendors.end(); ++i)
    {
        ObjectGuid vendorguid = *i;
        Creature* pCreature = bot->GetNPCIfCanInteractWith(vendorguid, UNIT_NPC_FLAG_VENDOR);
        if (!pCreature)
            continue;

        vendored = true;

        if (buyUseful)
        {
            // Items are evaluated from high-level to low level.
            // For each item the bot checks again if an item is usefull.
            // Bot will buy until no usefull items are left.

            VendorItemData const* tItems = pCreature->GetVendorItems();
            if (!tItems)
                continue;

            VendorItemList m_items_sorted = tItems->m_items;

            m_items_sorted.erase(std::remove_if(m_items_sorted.begin(), m_items_sorted.end(),
                                                [](VendorItem* i)
                                                {
                                                    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(i->item);
                                                    return !proto;
                                                }),
                                 m_items_sorted.end());

            if (m_items_sorted.empty())
                continue;

            StatsWeightCalculator calculator(bot);
            calculator.SetItemSetBonus(false);
            calculator.SetOverflowPenalty(false);

            std::sort(m_items_sorted.begin(), m_items_sorted.end(),
                [&calculator](VendorItem* i, VendorItem* j)
                {
                    ItemTemplate const* item1 = sObjectMgr->GetItemTemplate(i->item);
                    ItemTemplate const* item2 = sObjectMgr->GetItemTemplate(j->item);

                    if (!item1 || !item2)
                        return false;

                    float score1 = calculator.CalculateItem(item1->ItemId);
                    float score2 = calculator.CalculateItem(item2->ItemId);

                    // Fallback to itemlevel if either score is 0
                    if (score1 == 0 || score2 == 0)
                    {
                        score1 = item1->ItemLevel;
                        score2 = item2->ItemLevel;
                    }
                    return score1 > score2; // Sort in descending order (highest score first)
                });

            std::unordered_map<uint32, float> bestPurchasedItemScore;  // Track best item score per InventoryType

            for (auto& tItem : m_items_sorted)
            {
                uint32 maxPurchases = 1;  // Default to buying once
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(tItem->item);
                if (!proto)
                    continue;

                if (proto->Class == ITEM_CLASS_CONSUMABLE || proto->Class == ITEM_CLASS_PROJECTILE)
                {
                    maxPurchases = 10;  // Allow up to 10 purchases if it's a consumable or projectile
                }

                for (uint32 i = 0; i < maxPurchases; i++)
                {
                    ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", tItem->item);

                    uint32 invType = proto->InventoryType;

                    // Calculate item score
                    float newScore = calculator.CalculateItem(proto->ItemId);

                    // Skip if we already bought a better item for this slot
                    if (bestPurchasedItemScore.find(invType) != bestPurchasedItemScore.end() &&
                        bestPurchasedItemScore[invType] > newScore)
                    {
                        break;  // Skip lower-scoring items
                    }

                    // Check the bot's currently equipped item for this slot
                    uint8 dstSlot = botAI->FindEquipSlot(proto, NULL_SLOT, true);
                    Item* oldItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, dstSlot);

                    float oldScore = 0.0f;
                    if (oldItem)
                    {
                        ItemTemplate const* oldItemProto = oldItem->GetTemplate();
                        if (oldItemProto)
                            oldScore = calculator.CalculateItem(oldItemProto->ItemId);
                    }

                    // Skip if the bot already has a better or equal item equipped
                    if (oldScore > newScore)
                        break;

                    uint32 price = proto->BuyPrice;
                    price = uint32(floor(price * bot->GetReputationPriceDiscount(pCreature)));

                    NeedMoneyFor needMoneyFor = NeedMoneyFor::none;
                    switch (usage)
                    {
                        case ITEM_USAGE_REPLACE:
                        case ITEM_USAGE_EQUIP:
                        case ITEM_USAGE_BAD_EQUIP:
                        case ITEM_USAGE_BROKEN_EQUIP:
                            needMoneyFor = NeedMoneyFor::gear;
                            break;
                        case ITEM_USAGE_AMMO:
                            needMoneyFor = NeedMoneyFor::ammo;
                            break;
                        case ITEM_USAGE_QUEST:
                            needMoneyFor = NeedMoneyFor::anything;
                            break;
                        case ITEM_USAGE_USE:
                            needMoneyFor = NeedMoneyFor::consumables;
                            break;
                        case ITEM_USAGE_SKILL:
                            needMoneyFor = NeedMoneyFor::tradeskill;
                            break;
                        default:
                            break;
                    }

                    if (needMoneyFor == NeedMoneyFor::none)
                        break;

                    if (AI_VALUE2(uint32, "free money for", uint32(needMoneyFor)) < price)
                        break;

                    if (!BuyItem(tItems, vendorguid, proto))
                        break;

                    // Store the best item score per InventoryType
                    bestPurchasedItemScore[invType] = newScore;

                    if (needMoneyFor == NeedMoneyFor::gear)
                    {
                        botAI->DoSpecificAction("equip upgrades packet action");
                    }
                }
            }
        }
        else
        {
            if (itemIds.empty())
                return false;

            for (ItemIds::iterator i = itemIds.begin(); i != itemIds.end(); i++)
            {
                uint32 itemId = *i;
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
                if (!proto)
                    continue;

                result |= BuyItem(pCreature->GetVendorItems(), vendorguid, proto);

                if (!result)
                {
                    std::ostringstream out;
                    out << "Nobody sells " << ChatHelper::FormatItem(proto) << " nearby";
                    botAI->TellMaster(out.str());
                    continue;
                }

                ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", itemId);
                if (usage == ITEM_USAGE_REPLACE || usage == ITEM_USAGE_EQUIP ||
                    usage == ITEM_USAGE_BAD_EQUIP || usage == ITEM_USAGE_BROKEN_EQUIP)
                {
                    botAI->DoSpecificAction("equip upgrades packet action");
                    break;
                }
            }
        }
    }

    if (!vendored)
    {
        botAI->TellError("There are no vendors nearby");
        return false;
    }

    return true;
}

bool BuyAction::BuyFromAuctionHouse()
{
    if (!sPlayerbotAIConfig.enableAuctionHouseBotting)
        return false;

    GuidVector npcs = botAI->GetAiObjectContext()->GetValue<GuidVector>("nearest npcs")->Get();

    ObjectGuid auctioneerGuid;
    Creature* auctioneer = nullptr;
    for (ObjectGuid const& guid : npcs)
    {
        auctioneer = bot->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_AUCTIONEER);
        if (auctioneer)
        {
            auctioneerGuid = guid;
            break;
        }
    }

    if (!auctioneer)
    {
        botAI->TellError("There are no auctioneers nearby");
        return false;
    }

    AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMap(auctioneer->GetFaction());
    if (!auctionHouse)
        return false;

    AuctionEntry* bestAuction = nullptr;
    uint32 bestPrice = std::numeric_limits<uint32>::max();
    uint32 scanned = 0;

    for (auto itr = auctionHouse->GetAuctionsBegin(); itr != auctionHouse->GetAuctionsEnd(); ++itr)
    {
        if (++scanned > 300)
            break;

        AuctionEntry* auction = itr->second;
        if (!auction || !auction->buyout)
            continue;

        if (auction->owner == bot->GetGUID())
            continue;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(auction->item_template);
        if (!proto)
            continue;

        NeedMoneyFor needMoneyFor = NeedMoneyFor::none;
        if (!IsAuctionItemUseful(proto, auction->buyout, needMoneyFor))
            continue;

        if (auction->buyout < bestPrice)
        {
            bestPrice = auction->buyout;
            bestAuction = auction;
        }
    }

    if (!bestAuction)
        return false;

    return BuyAuction(auctioneerGuid, bestAuction);
}

bool BuyAction::BuyAuction(ObjectGuid auctioneerGuid, AuctionEntry* auction)
{
    if (!auction || !auction->buyout)
        return false;

    uint32 const auctionId = auction->Id;
    uint32 const buyout = auction->buyout;
    uint32 const itemTemplateId = auction->item_template;

    Creature* auctioneer = bot->GetNPCIfCanInteractWith(auctioneerGuid, UNIT_NPC_FLAG_AUCTIONEER);
    if (!auctioneer)
        return false;

    AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMap(auctioneer->GetFaction());
    if (!auctionHouse || !auctionHouse->GetAuction(auctionId))
        return false;

    uint32 botMoney = bot->GetMoney();

    WorldPacket packet(CMSG_AUCTION_PLACE_BID);
    packet << auctioneerGuid;
    packet << auctionId;
    packet << buyout;

    bot->GetSession()->HandleAuctionPlaceBid(packet);

    if (botAI->HasCheat(BotCheatMask::gold))
        bot->SetMoney(botMoney);

    if (auctionHouse->GetAuction(auctionId))
    {
        LOG_DEBUG("playerbots", "{}: failed to buy auction {} via {} (buyout={})",
            bot->GetName(), auctionId, auctioneerGuid.ToString(), buyout);
        return false;
    }

    ItemTemplate const* boughtItemProto = sObjectMgr->GetItemTemplate(itemTemplateId);

    LOG_DEBUG("playerbots", "{}: bought {} from auction house via {} for {}",
        bot->GetName(), boughtItemProto ? boughtItemProto->Name1 : std::to_string(itemTemplateId),
        auctioneerGuid.ToString(), buyout);

    std::ostringstream out;
    out << "Buying from auction house "
        << (boughtItemProto ? ChatHelper::FormatItem(boughtItemProto) : std::to_string(itemTemplateId))
        << " for " << buyout;
    botAI->TellMaster(out.str());

    return true;
}

bool BuyAction::IsAuctionItemUseful(ItemTemplate const* proto, uint32 buyout,
                                    NeedMoneyFor& needMoneyFor)
{
    if (!proto || !buyout)
        return false;

    ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", proto->ItemId);
    needMoneyFor = GetBudgetTypeForUsage(usage);
    if (needMoneyFor == NeedMoneyFor::none)
        return false;

    if (AI_VALUE2(uint32, "free money for", uint32(needMoneyFor)) < buyout)
        return false;

    if ((proto->Class == ITEM_CLASS_CONSUMABLE || proto->Class == ITEM_CLASS_PROJECTILE) &&
        bot->GetItemCount(proto->ItemId, true) > 200)
        return false;

    return true;
}

NeedMoneyFor BuyAction::GetBudgetTypeForUsage(ItemUsage usage) const
{
    switch (usage)
    {
        case ITEM_USAGE_REPLACE:
        case ITEM_USAGE_EQUIP:
        case ITEM_USAGE_BAD_EQUIP:
        case ITEM_USAGE_BROKEN_EQUIP:
            return NeedMoneyFor::gear;
        case ITEM_USAGE_AMMO:
            return NeedMoneyFor::ammo;
        case ITEM_USAGE_QUEST:
            return NeedMoneyFor::anything;
        case ITEM_USAGE_USE:
            return NeedMoneyFor::consumables;
        case ITEM_USAGE_SKILL:
            return NeedMoneyFor::tradeskill;
        default:
            return NeedMoneyFor::none;
    }
}

bool BuyAction::BuyItem(VendorItemData const* tItems, ObjectGuid vendorguid, ItemTemplate const* proto)
{
    if (!tItems || !proto)
        return false;

    uint32 itemId = proto->ItemId;
    uint32 oldCount = bot->GetItemCount(itemId, false);

    for (uint32 slot = 0; slot < tItems->GetItemCount(); ++slot)
    {
        if (tItems->GetItem(slot)->item != itemId)
            continue;

        uint32 botMoney = bot->GetMoney();
        if (botAI->HasCheat(BotCheatMask::gold))
            bot->SetMoney(10000000);

        bot->BuyItemFromVendorSlot(vendorguid, slot, itemId, 1, NULL_BAG, NULL_SLOT);

        if (botAI->HasCheat(BotCheatMask::gold))
            bot->SetMoney(botMoney);

        uint32 newCount = bot->GetItemCount(itemId, false);
        if (newCount > oldCount)
        {
            std::ostringstream out;
            out << "Buying " << ChatHelper::FormatItem(proto);
            botAI->TellMaster(out.str());
            return true;
        }

        return false;
    }

    return false;
}
