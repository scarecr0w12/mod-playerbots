/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "SellAction.h"

#include "AuctionHouseMgr.h"
#include "Event.h"
#include "ItemUsageValue.h"
#include "ItemVisitors.h"
#include "Playerbots.h"
#include "ItemPackets.h"

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
        if (usage != ITEM_USAGE_VENDOR && usage != ITEM_USAGE_AH)
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

    botAI->TellError("Usage: s gray/*/vendor/[item link]");
    return false;
}

bool SellAction::SellToAuctionHouse(Item* item)
{
    if (!item)
        return false;

    ItemTemplate const* proto = item->GetTemplate();
    if (!proto || !item->CanBeTraded())
        return false;

    if (proto->Bonding == BIND_WHEN_PICKED_UP || proto->Bonding == BIND_QUEST_ITEM)
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
        return false;

    uint32 unitPrice = proto->BuyPrice ? proto->BuyPrice / 4 : proto->SellPrice * 5;
    if (!unitPrice)
        unitPrice = 1;

    uint32 itemCount = item->GetCount();
    uint32 startBid = std::max<uint32>(itemCount * unitPrice, 100);
    uint32 buyout = std::max<uint32>(startBid + 1, startBid * 12 / 10);
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
        return false;

    std::ostringstream out;
    out << "Posting to auction house " << chat->FormatItem(proto)
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
