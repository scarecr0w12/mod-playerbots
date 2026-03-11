/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_AUCTIONHOUSEPOLICY_H
#define _PLAYERBOT_AUCTIONHOUSEPOLICY_H

#include <algorithm>
#include <mutex>
#include <unordered_map>

#include "AuctionHouseMgr.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectGuid.h"
#include "PlayerbotAIConfig.h"

struct PlayerbotAuctionItemPolicy
{
    bool sellable = true;
    uint8 chanceToSell = 100;
    uint16 minStackCount = 0;
    uint16 maxStackCount = 0;
    uint16 minBidPct = 100;
    uint16 buyoutMinPct = 110;
    uint16 buyoutMaxPct = 133;
    uint8 undercutChance = 15;
    uint16 marketPriceWeightPct = 75;
};

struct PlayerbotAuctionMarketSnapshot
{
    uint32 minUnitBuyout = 0;
    uint32 avgUnitBuyout = 0;
    uint32 sampleCount = 0;

    [[nodiscard]] bool HasData() const
    {
        return sampleCount > 0 && minUnitBuyout > 0;
    }
};

class PlayerbotAuctionHousePolicyMgr
{
public:
    static PlayerbotAuctionHousePolicyMgr& instance()
    {
        static PlayerbotAuctionHousePolicyMgr instance;
        return instance;
    }

    void Initialize()
    {
        std::lock_guard<std::mutex> guard(_lock);

        _policies.clear();
        _tableAvailable = TableExistsLocked();
        if (!_tableAvailable)
        {
            LOG_WARN("playerbots", "playerbots_auction_item_policy table not found. Using built-in auction defaults.");
            return;
        }

        SyncSellableItemsLocked();

        QueryResult result = PlayerbotsDatabase.Query(
            "SELECT `item_id`, `sellable`, `chance_to_sell`, `min_stack_count`, `max_stack_count`, "
            "`min_bid_pct`, `buyout_min_pct`, `buyout_max_pct`, `undercut_chance`, `market_price_weight_pct` "
            "FROM `playerbots_auction_item_policy`");

        if (!result)
        {
            LOG_INFO("playerbots", "Loaded 0 playerbots auction item policies.");
            return;
        }

        do
        {
            Field* fields = result->Fetch();

            PlayerbotAuctionItemPolicy policy;
            policy.sellable = fields[1].Get<uint8>() != 0;
            policy.chanceToSell = std::min<uint32>(100, fields[2].Get<uint32>());
            policy.minStackCount = fields[3].Get<uint16>();
            policy.maxStackCount = fields[4].Get<uint16>();
            policy.minBidPct = std::max<uint32>(1, fields[5].Get<uint32>());
            policy.buyoutMinPct = std::max<uint32>(100, fields[6].Get<uint32>());
            policy.buyoutMaxPct = std::max<uint32>(policy.buyoutMinPct, fields[7].Get<uint32>());
            policy.undercutChance = std::min<uint32>(100, fields[8].Get<uint32>());
            policy.marketPriceWeightPct = std::min<uint32>(100, fields[9].Get<uint32>());

            _policies[fields[0].Get<uint32>()] = policy;
        } while (result->NextRow());

        LOG_INFO("playerbots", "Loaded {} playerbots auction item policies.", _policies.size());
    }

    [[nodiscard]] PlayerbotAuctionItemPolicy GetPolicy(uint32 itemId) const
    {
        std::lock_guard<std::mutex> guard(_lock);

        auto itr = _policies.find(itemId);
        if (itr != _policies.end())
            return itr->second;

        return MakeDefaultPolicy();
    }

    [[nodiscard]] bool IsSellable(uint32 itemId) const
    {
        return GetPolicy(itemId).sellable;
    }

private:
    [[nodiscard]] PlayerbotAuctionItemPolicy MakeDefaultPolicy() const
    {
        PlayerbotAuctionItemPolicy policy;
        policy.buyoutMinPct = std::max<uint32>(100, sPlayerbotAIConfig.auctionHouseBuyoutMinPct);
        policy.buyoutMaxPct = std::max<uint32>(policy.buyoutMinPct, sPlayerbotAIConfig.auctionHouseBuyoutMaxPct);
        policy.undercutChance = std::min<uint32>(100, sPlayerbotAIConfig.auctionHouseUndercutChance);
        return policy;
    }

    [[nodiscard]] bool TableExistsLocked() const
    {
        std::string const dbName = PlayerbotsDatabase.GetConnectionInfo()->database;
        QueryResult result = PlayerbotsDatabase.Query(
            "SELECT EXISTS(SELECT 1 FROM information_schema.tables WHERE table_schema = '{}' "
            "AND table_name = 'playerbots_auction_item_policy')",
            dbName);

        if (!result)
            return false;

        return result->Fetch()[0].Get<uint32>() != 0;
    }

    void SyncSellableItemsLocked() const
    {
        std::string const worldDbName = WorldDatabase.GetConnectionInfo()->database;
        PlayerbotsDatabase.Execute(
            "INSERT IGNORE INTO `playerbots_auction_item_policy` "
            "(`item_id`, `sellable`, `chance_to_sell`, `min_stack_count`, `max_stack_count`, "
            "`min_bid_pct`, `buyout_min_pct`, `buyout_max_pct`, `undercut_chance`, `market_price_weight_pct`) "
            "SELECT `entry`, 1, 100, 0, 0, 100, {}, {}, {}, 75 FROM `" + worldDbName + "`.`item_template` "
            "WHERE `SellPrice` > 0",
            std::max<uint32>(100, sPlayerbotAIConfig.auctionHouseBuyoutMinPct),
            std::max<uint32>(std::max<uint32>(100, sPlayerbotAIConfig.auctionHouseBuyoutMinPct),
                sPlayerbotAIConfig.auctionHouseBuyoutMaxPct),
            std::min<uint32>(100, sPlayerbotAIConfig.auctionHouseUndercutChance));
    }

private:
    mutable std::mutex _lock;
    std::unordered_map<uint32, PlayerbotAuctionItemPolicy> _policies;
    bool _tableAvailable = false;
};

inline PlayerbotAuctionMarketSnapshot GetPlayerbotAuctionMarketSnapshot(
    AuctionHouseObject* auctionHouse, uint32 itemId, ObjectGuid owner = ObjectGuid())
{
    PlayerbotAuctionMarketSnapshot snapshot;
    if (!auctionHouse || !itemId)
        return snapshot;

    uint64 totalUnitBuyout = 0;
    for (auto itr = auctionHouse->GetAuctionsBegin(); itr != auctionHouse->GetAuctionsEnd(); ++itr)
    {
        AuctionEntry const* auction = itr->second;
        if (!auction || auction->item_template != itemId || !auction->buyout || !auction->itemCount)
            continue;

        if (!owner.IsEmpty() && auction->owner == owner)
            continue;

        uint32 unitBuyout = std::max<uint32>(1, auction->buyout / auction->itemCount);
        if (!snapshot.minUnitBuyout || unitBuyout < snapshot.minUnitBuyout)
            snapshot.minUnitBuyout = unitBuyout;

        totalUnitBuyout += unitBuyout;
        ++snapshot.sampleCount;
    }

    if (snapshot.sampleCount)
        snapshot.avgUnitBuyout = std::max<uint32>(1, totalUnitBuyout / snapshot.sampleCount);

    return snapshot;
}

inline uint32 GetPlayerbotAuctionReferenceUnitPrice(PlayerbotAuctionMarketSnapshot const& snapshot)
{
    if (!snapshot.HasData())
        return 0;

    if (snapshot.sampleCount == 1)
        return snapshot.minUnitBuyout;

    return std::max<uint32>(1, (snapshot.minUnitBuyout + snapshot.avgUnitBuyout) / 2);
}

#define sPlayerbotAuctionHousePolicyMgr PlayerbotAuctionHousePolicyMgr::instance()

#endif