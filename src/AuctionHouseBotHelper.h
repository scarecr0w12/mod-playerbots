/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_AUCTIONHOUSEBOTHELPER_H
#define _PLAYERBOT_AUCTIONHOUSEBOTHELPER_H

#include "AiObjectContext.h"
#include "Bag.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "ItemUsageValue.h"
#include "Player.h"

inline bool IsAuctionHouseMaterial(ItemTemplate const* proto)
{
    if (!proto)
        return false;

    switch (proto->Class)
    {
        case ITEM_CLASS_TRADE_GOODS:
        case ITEM_CLASS_GEM:
            return true;
        case ITEM_CLASS_MISC:
            return proto->SubClass != ITEM_SUBCLASS_REAGENT;
        default:
            return false;
    }
}

inline bool IsPreferredAuctionHouseItem(ItemTemplate const* proto)
{
    if (!proto)
        return false;

    if (proto->Quality >= ITEM_QUALITY_UNCOMMON)
        return true;

    if (IsAuctionHouseMaterial(proto))
        return true;

    if (proto->Bonding != NO_BIND || proto->Quality < ITEM_QUALITY_NORMAL)
        return false;

    switch (proto->Class)
    {
        case ITEM_CLASS_CONTAINER:
        case ITEM_CLASS_CONSUMABLE:
        case ITEM_CLASS_ARMOR:
        case ITEM_CLASS_WEAPON:
            return true;
        default:
            return false;
    }
}

inline uint32 CountPreferredAuctionHouseItems(Player* bot, AiObjectContext* context)
{
    if (!bot || !context)
        return 0;

    uint32 count = 0;

    auto countItem = [&](Item* item)
    {
        if (!item)
            return;

        ItemTemplate const* proto = item->GetTemplate();
        if (!IsPreferredAuctionHouseItem(proto))
            return;

        ItemUsage usage = context->GetValue<ItemUsage>("item usage", item->GetEntry())->Get();
        if (usage == ITEM_USAGE_AH)
            count += item->GetCount();
    };

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        countItem(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));

    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag* bag = bot->GetBagByPos(bagSlot);
        if (!bag)
            continue;

        for (uint32 itemSlot = 0; itemSlot < bag->GetBagSize(); ++itemSlot)
            countItem(bag->GetItemByPos(itemSlot));
    }

    return count;
}

#endif
