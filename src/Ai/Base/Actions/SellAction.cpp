/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "SellAction.h"

#include "AuctionHouseBotHelper.h"
#include "AuctionHouseMgr.h"
#include "Db/PlayerbotSpellRepository.h"
#include "Event.h"
#include "ItemUsageValue.h"
#include "ItemVisitors.h"
#include "Log.h"
#include "Playerbots.h"
#include "PlayerbotAuctionHousePolicy.h"
#include "ItemPackets.h"

#include <algorithm>

namespace
{
constexpr uint32 AuctionHouseMaterialMinCount = 5;

uint32 RoundAuctionPrice(double price)
{
    if (price <= 1.0)
        return 1;

    if (price < 100.0)
        return uint32(price);

    if (price < 10000.0)
        return uint32(price / 100.0) * 100;

    if (price < 100000.0)
        return uint32(price / 1000.0) * 1000;

    return uint32(price / 10000.0) * 10000;
}

uint32 GetAuctionStackCount(Item* item, PlayerbotAuctionItemPolicy const& policy)
{
    if (!item)
        return 0;

    uint32 itemCount = item->GetCount();
    if (!itemCount)
        return 0;

    uint32 maxStackCount = std::min<uint32>(itemCount, item->GetMaxStackCount());
    if (policy.maxStackCount)
        maxStackCount = std::min<uint32>(maxStackCount, std::max<uint32>(1, policy.maxStackCount));

    if (!sPlayerbotAIConfig.auctionHouseRandomStackSize)
        return maxStackCount ? maxStackCount : itemCount;

    if (maxStackCount <= 1)
        return 1;

    uint32 minStackCount = 1;
    if (IsAuctionHouseMaterial(item->GetTemplate()) && itemCount >= AuctionHouseMaterialMinCount)
        minStackCount = std::min<uint32>(AuctionHouseMaterialMinCount, maxStackCount);

    if (policy.minStackCount)
        minStackCount = std::min<uint32>(std::max<uint32>(1, policy.minStackCount), maxStackCount);

    if (maxStackCount <= minStackCount)
        return maxStackCount;

    return urand(minStackCount, maxStackCount);
}

uint32 GetAuctionUnitPrice(Player* bot, ItemTemplate const* proto, AuctionHouseObject* auctionHouse,
    PlayerbotAuctionItemPolicy const& policy, PlayerbotAuctionMarketSnapshot* marketSnapshot = nullptr)
{
    if (!bot || !proto)
        return 0;

    uint32 marketWeight = std::min<uint32>(100, policy.marketPriceWeightPct);

    uint32 unitPrice = 0;

    if (proto->BuyPrice)
        unitPrice = RoundAuctionPrice(proto->BuyPrice * sRandomPlayerbotMgr.GetBuyMultiplier(bot));
    else if (proto->SellPrice)
        unitPrice = RoundAuctionPrice(proto->SellPrice * std::max(1.0, sRandomPlayerbotMgr.GetSellMultiplier(bot)));
    else
        unitPrice = 1;

    if (!marketWeight)
        return std::max<uint32>(1, unitPrice);

    PlayerbotAuctionMarketSnapshot snapshot =
        GetPlayerbotAuctionMarketSnapshot(auctionHouse, proto->ItemId, bot->GetGUID());
    if (marketSnapshot)
        *marketSnapshot = snapshot;

    uint32 marketUnitPrice = GetPlayerbotAuctionReferenceUnitPrice(snapshot);
    if (!marketUnitPrice)
        return std::max<uint32>(1, unitPrice);

    return std::max<uint32>(1,
        RoundAuctionPrice((double(unitPrice) * (100 - marketWeight) + double(marketUnitPrice) * marketWeight) / 100.0));
}

bool HasNearbyAuctioneer(Player* bot, GuidVector const& npcs, ObjectGuid& auctioneerGuid)
{
    for (ObjectGuid const& guid : npcs)
    {
        if (!bot->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_AUCTIONEER))
            continue;

        auctioneerGuid = guid;
        return true;
    }

    return false;
}

bool IsSpellReagentItem(ItemTemplate const* proto)
{
    if (!proto)
        return false;

    return proto->Class == ITEM_CLASS_REAGENT ||
           (proto->Class == ITEM_CLASS_MISC && proto->SubClass == ITEM_SUBCLASS_REAGENT);
}
}

class SellItemsVisitor : public IterateItemsVisitor
{
public:
    SellItemsVisitor(SellAction* action) : IterateItemsVisitor(), action(action) {}

    bool Visit(Item* item) override
    {
        action->Sell(item);
        return true;
    }

private:
    SellAction* action;
};

class SellGrayItemsVisitor : public SellItemsVisitor
{
public:
    SellGrayItemsVisitor(SellAction* action) : SellItemsVisitor(action) {}

    bool Visit(Item* item) override
    {
        if (item->GetTemplate()->Quality != ITEM_QUALITY_POOR)
            return true;

        return SellItemsVisitor::Visit(item);
    }
};

class SellVendorItemsVisitor : public SellItemsVisitor
{
public:
    SellVendorItemsVisitor(SellAction* action, AiObjectContext* con) : SellItemsVisitor(action) { context = con; }

    AiObjectContext* context;

    bool Visit(Item* item) override
    {
        ItemUsage usage = context->GetValue<ItemUsage>("item usage", item->GetEntry())->Get();
        if (usage != ITEM_USAGE_VENDOR)
            return true;

        return SellItemsVisitor::Visit(item);
    }
};

class SellAhItemsVisitor : public SellItemsVisitor
{
public:
    SellAhItemsVisitor(SellAction* action, AiObjectContext* con)
        : SellItemsVisitor(action), action(action), context(con) { }

    bool Visit(Item* item) override
    {
        if (sPlayerbotAIConfig.IsInAuctionHouseExcludedItemList(item->GetEntry()))
            return true;

        if (!sPlayerbotAuctionHousePolicyMgr.IsSellable(item->GetEntry()))
            return true;

        ItemUsage usage = context->GetValue<ItemUsage>("item usage", item->GetEntry())->Get();
        if (usage != ITEM_USAGE_AH)
            return true;

        action->SellToAuctionHouse(item);
        return true;
    }

private:
    SellAction* action;
    AiObjectContext* context;
};

bool SellAction::Execute(Event event)
{
    std::string const text = event.getParam();
    if (text == "gray" || text == "*")
    {
        SellGrayItemsVisitor visitor(this);
        IterateItems(&visitor);
        return true;
    }

    if (text == "vendor")
    {
        SellVendorItemsVisitor visitor(this, context);
        IterateItems(&visitor);
        return true;
    }

    if (text == "auction")
    {
        SellAhItemsVisitor visitor(this, context);
        IterateItems(&visitor);
        return true;
    }

    if (text != "")
    {
        std::vector<Item*> items = parseItems(text, ITERATE_ITEMS_IN_BAGS);
        for (Item* item : items)
        {
            Sell(item);
        }
        return true;
    }

    botAI->TellError("Usage: s gray/*/vendor/auction/[item link]");
    return false;
}

bool SellAction::SellToAuctionHouse(Item* item)
{
    if (!item)
        return false;

    if (sPlayerbotAIConfig.IsInAuctionHouseExcludedItemList(item->GetEntry()))
        return false;

    if (PlayerbotSpellRepository::Instance().IsItemBuyable(item->GetEntry()) &&
        IsSpellReagentItem(item->GetTemplate()))
        return false;

    PlayerbotAuctionItemPolicy policy = sPlayerbotAuctionHousePolicyMgr.GetPolicy(item->GetEntry());
    if (!policy.sellable)
        return false;

    if (!policy.chanceToSell || urand(1, 100) > policy.chanceToSell)
        return false;

    ItemTemplate const* proto = item->GetTemplate();
    if (!proto || !item->CanBeTraded())
        return false;

    if (proto->Quality == ITEM_QUALITY_POOR)
        return false;

    if (proto->Class == ITEM_CLASS_PROJECTILE)
        return false;

    if (proto->Quality == ITEM_QUALITY_NORMAL && !IsAuctionHouseMaterial(proto))
        return false;

    if (proto->Bonding == BIND_WHEN_PICKED_UP || proto->Bonding == BIND_QUEST_ITEM)
        return false;

    GuidVector npcs = botAI->GetAiObjectContext()->GetValue<GuidVector>("nearest npcs")->Get();

    ObjectGuid auctioneerGuid;
    if (!HasNearbyAuctioneer(bot, npcs, auctioneerGuid))
    {
        LOG_DEBUG("playerbots", "{}: cannot post item {} to auction house - no nearby auctioneer",
            bot->GetName(), proto->ItemId);
        return false;
    }

    Creature* auctioneer = bot->GetNPCIfCanInteractWith(auctioneerGuid, UNIT_NPC_FLAG_AUCTIONEER);
    if (!auctioneer)
        return false;

    AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMap(auctioneer->GetFaction());

    uint32 itemCount = GetAuctionStackCount(item, policy);
    if (!itemCount)
        return false;

    PlayerbotAuctionMarketSnapshot marketSnapshot;
    uint32 unitPrice = GetAuctionUnitPrice(bot, proto, auctionHouse, policy, &marketSnapshot);
    if (!unitPrice)
        return false;

    if (policy.undercutChance && urand(1, 100) <= policy.undercutChance)
    {
        uint32 minPct = std::max<uint32>(100, sPlayerbotAIConfig.auctionHouseUndercutMinPct);
        uint32 maxPct = std::max<uint32>(minPct, sPlayerbotAIConfig.auctionHouseUndercutMaxPct);
        uint32 anchorUnitPrice = marketSnapshot.HasData() ? marketSnapshot.minUnitBuyout : unitPrice;
        unitPrice = std::max<uint32>(1, RoundAuctionPrice(double(anchorUnitPrice) * 100.0 / urand(minPct, maxPct)));
    }

    uint32 startBid = std::max<uint32>(sPlayerbotAIConfig.auctionHouseMinBidPrice,
        RoundAuctionPrice(double(itemCount) * unitPrice * std::max<uint32>(1, policy.minBidPct) / 100.0));
    uint32 minBuyoutPct = std::max<uint32>(100, policy.buyoutMinPct);
    uint32 maxBuyoutPct = std::max<uint32>(minBuyoutPct, policy.buyoutMaxPct);
    uint32 buyout = RoundAuctionPrice(double(startBid) * urand(minBuyoutPct, maxBuyoutPct) / 100.0);
    if (buyout <= startBid)
        buyout = startBid + 1;

    uint32 etime = uint32(12 * HOUR / MINUTE);

    uint32 oldCount = bot->GetItemCount(proto->ItemId, true);
    uint32 botMoney = bot->GetMoney();

    WorldPacket packet(CMSG_AUCTION_SELL_ITEM);
    packet << auctioneerGuid;
    packet << uint32(1);
    packet << item->GetGUID();
    packet << itemCount;
    packet << startBid;
    packet << buyout;
    packet << etime;

    bot->GetSession()->HandleAuctionSellItem(packet);

    if (botAI->HasCheat(BotCheatMask::gold))
        bot->SetMoney(botMoney);

    if (bot->GetItemCount(proto->ItemId, true) >= oldCount)
    {
        LOG_DEBUG("playerbots",
            "{}: failed to post {} x{} to auction house via {} (bid={}, buyout={})",
            bot->GetName(), proto->Name1, itemCount, auctioneerGuid.ToString(), startBid, buyout);
        return false;
    }

    LOG_DEBUG("playerbots",
        "{}: posted {} x{} to auction house via {} (bid={}, buyout={})",
        bot->GetName(), proto->Name1, itemCount, auctioneerGuid.ToString(), startBid, buyout);

    std::ostringstream out;
    out << "Posting to auction house " << chat->FormatItem(proto, itemCount)
        << " for " << startBid << ".." << buyout;
    botAI->TellMaster(out);

    return true;
}

void SellAction::Sell(FindItemVisitor* visitor)
{
    IterateItems(visitor);
    std::vector<Item*> items = visitor->GetResult();
    for (Item* item : items)
    {
        Sell(item);
    }
}

void SellAction::Sell(Item* item)
{
    if (!item)
        return;

    ItemUsage usage = context->GetValue<ItemUsage>("item usage", item->GetEntry())->Get();
    if (usage == ITEM_USAGE_AH && SellToAuctionHouse(item))
        return;

    std::ostringstream out;

    GuidVector vendors = botAI->GetAiObjectContext()->GetValue<GuidVector>("nearest npcs")->Get();

    for (ObjectGuid const vendorguid : vendors)
    {
        Creature* pCreature = bot->GetNPCIfCanInteractWith(vendorguid, UNIT_NPC_FLAG_VENDOR);
        if (!pCreature)
            continue;

        ObjectGuid itemguid = item->GetGUID();
        uint32 count = item->GetCount();

        uint32 botMoney = bot->GetMoney();

        WorldPacket p(CMSG_SELL_ITEM);
        p << vendorguid << itemguid << count;

        WorldPackets::Item::SellItem nicePacket(std::move(p));
        nicePacket.Read();
        bot->GetSession()->HandleSellItemOpcode(nicePacket);

        if (botAI->HasCheat(BotCheatMask::gold))
        {
            bot->SetMoney(botMoney);
        }

        out << "Selling " << chat->FormatItem(item->GetTemplate());
        botAI->TellMaster(out);

        bot->PlayDistanceSound(120);
        break;
    }
}
