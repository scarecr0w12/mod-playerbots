/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_OPERATIONS_H
#define _PLAYERBOT_OPERATIONS_H

#include "Group.h"
#include "GroupMgr.h"
#include "GuildMgr.h"
#include "AuctionHouseMgr.h"
#include "ChatHelper.h"
#include "Item.h"
#include "Playerbots.h"
#include "ObjectAccessor.h"
#include "PlayerbotOperation.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotMgr.h"
#include "PlayerbotRepository.h"
#include "RandomPlayerbotMgr.h"
#include "UseMeetingStoneAction.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

// Group invite operation
class GroupInviteOperation : public PlayerbotOperation
{
public:
    GroupInviteOperation(ObjectGuid botGuid, ObjectGuid targetGuid)
        : m_botGuid(botGuid), m_targetGuid(targetGuid)
    {
    }

    bool Execute() override
    {
        Player* bot = ObjectAccessor::FindPlayer(m_botGuid);
        Player* target = ObjectAccessor::FindPlayer(m_targetGuid);

        if (!bot || !target)
        {
            LOG_DEBUG("playerbots", "GroupInviteOperation: Bot or target not found");
            return false;
        }

        // Check if target is already in a group
        if (target->GetGroup())
        {
            LOG_DEBUG("playerbots", "GroupInviteOperation: Target {} is already in a group", target->GetName());
            return false;
        }

        Group* group = bot->GetGroup();

        // Create group if bot doesn't have one
        if (!group)
        {
            group = new Group;
            if (!group->Create(bot))
            {
                delete group;
                LOG_ERROR("playerbots", "GroupInviteOperation: Failed to create group for bot {}", bot->GetName());
                return false;
            }
            sGroupMgr->AddGroup(group);
            LOG_DEBUG("playerbots", "GroupInviteOperation: Created new group for bot {}", bot->GetName());
        }

        // Convert to raid if needed (more than 5 members)
        if (!group->isRaidGroup() && group->GetMembersCount() >= 5)
        {
            group->ConvertToRaid();
            LOG_DEBUG("playerbots", "GroupInviteOperation: Converted group to raid");
        }

        // Add member to group
        if (group->AddMember(target))
        {
            LOG_DEBUG("playerbots", "GroupInviteOperation: Successfully added {} to group", target->GetName());
            if (sPlayerbotAIConfig.summonWhenGroup && target->GetDistance(bot) > sPlayerbotAIConfig.sightDistance)
            {
                PlayerbotAI* targetAI = sPlayerbotsMgr.GetPlayerbotAI(target);
                if (targetAI)
                {
                    SummonAction summonAction(targetAI, "group summon");
                    summonAction.Teleport(bot, target, true);
                }
            }
            return true;
        }
        else
        {
            LOG_ERROR("playerbots", "GroupInviteOperation: Failed to add {} to group", target->GetName());
            return false;
        }
    }

    ObjectGuid GetBotGuid() const override { return m_botGuid; }

    uint32 GetPriority() const override { return 50; }  // High priority (player-facing)

    std::string GetName() const override { return "GroupInvite"; }

    bool IsValid() const override
    {
        // Check if bot still exists and is online
        Player* bot = ObjectAccessor::FindPlayer(m_botGuid);
        Player* target = ObjectAccessor::FindPlayer(m_targetGuid);
        return bot && target;
    }

private:
    ObjectGuid m_botGuid;
    ObjectGuid m_targetGuid;
};

// Remove member from group
class GroupRemoveMemberOperation : public PlayerbotOperation
{
public:
    GroupRemoveMemberOperation(ObjectGuid botGuid, ObjectGuid targetGuid)
        : m_botGuid(botGuid), m_targetGuid(targetGuid)
    {
    }

    bool Execute() override
    {
        Player* bot = ObjectAccessor::FindPlayer(m_botGuid);
        Player* target = ObjectAccessor::FindPlayer(m_targetGuid);

        if (!bot || !target)
            return false;

        Group* group = bot->GetGroup();
        if (!group)
        {
            LOG_DEBUG("playerbots", "GroupRemoveMemberOperation: Bot is not in a group");
            return false;
        }

        if (!group->IsMember(target->GetGUID()))
        {
            LOG_DEBUG("playerbots", "GroupRemoveMemberOperation: Target is not in bot's group");
            return false;
        }

        group->RemoveMember(target->GetGUID());
        LOG_DEBUG("playerbots", "GroupRemoveMemberOperation: Removed {} from group", target->GetName());
        return true;
    }

    ObjectGuid GetBotGuid() const override { return m_botGuid; }

    uint32 GetPriority() const override { return 50; }

    std::string GetName() const override { return "GroupRemoveMember"; }

    bool IsValid() const override
    {
        Player* bot = ObjectAccessor::FindPlayer(m_botGuid);
        return bot != nullptr;
    }

private:
    ObjectGuid m_botGuid;
    ObjectGuid m_targetGuid;
};

// Convert group to raid
class GroupConvertToRaidOperation : public PlayerbotOperation
{
public:
    GroupConvertToRaidOperation(ObjectGuid botGuid) : m_botGuid(botGuid) {}

    bool Execute() override
    {
        Player* bot = ObjectAccessor::FindPlayer(m_botGuid);
        if (!bot)
            return false;

        Group* group = bot->GetGroup();
        if (!group)
        {
            LOG_DEBUG("playerbots", "GroupConvertToRaidOperation: Bot is not in a group");
            return false;
        }

        if (group->isRaidGroup())
        {
            LOG_DEBUG("playerbots", "GroupConvertToRaidOperation: Group is already a raid");
            return true;  // Success - already in desired state
        }

        group->ConvertToRaid();
        LOG_DEBUG("playerbots", "GroupConvertToRaidOperation: Converted group to raid");
        return true;
    }

    ObjectGuid GetBotGuid() const override { return m_botGuid; }

    uint32 GetPriority() const override { return 50; }

    std::string GetName() const override { return "GroupConvertToRaid"; }

    bool IsValid() const override
    {
        Player* bot = ObjectAccessor::FindPlayer(m_botGuid);
        return bot != nullptr;
    }

private:
    ObjectGuid m_botGuid;
};

// Set group leader
class GroupSetLeaderOperation : public PlayerbotOperation
{
public:
    GroupSetLeaderOperation(ObjectGuid botGuid, ObjectGuid newLeaderGuid)
        : m_botGuid(botGuid), m_newLeaderGuid(newLeaderGuid)
    {
    }

    bool Execute() override
    {
        Player* bot = ObjectAccessor::FindPlayer(m_botGuid);
        Player* newLeader = ObjectAccessor::FindPlayer(m_newLeaderGuid);

        if (!bot || !newLeader)
            return false;

        Group* group = bot->GetGroup();
        if (!group)
        {
            LOG_DEBUG("playerbots", "GroupSetLeaderOperation: Bot is not in a group");
            return false;
        }

        if (!group->IsMember(newLeader->GetGUID()))
        {
            LOG_DEBUG("playerbots", "GroupSetLeaderOperation: New leader is not in the group");
            return false;
        }

        group->ChangeLeader(newLeader->GetGUID());
        LOG_DEBUG("playerbots", "GroupSetLeaderOperation: Changed leader to {}", newLeader->GetName());
        return true;
    }

    ObjectGuid GetBotGuid() const override { return m_botGuid; }

    uint32 GetPriority() const override { return 50; }

    std::string GetName() const override { return "GroupSetLeader"; }

    bool IsValid() const override
    {
        Player* bot = ObjectAccessor::FindPlayer(m_botGuid);
        Player* newLeader = ObjectAccessor::FindPlayer(m_newLeaderGuid);
        return bot && newLeader;
    }

private:
    ObjectGuid m_botGuid;
    ObjectGuid m_newLeaderGuid;
};

// Form arena group
class ArenaGroupFormationOperation : public PlayerbotOperation
{
public:
    ArenaGroupFormationOperation(ObjectGuid leaderGuid, std::vector<ObjectGuid> memberGuids,
                                 uint32 requiredSize, uint32 arenaTeamId, std::string arenaTeamName)
        : m_leaderGuid(leaderGuid), m_memberGuids(memberGuids),
          m_requiredSize(requiredSize), m_arenaTeamId(arenaTeamId), m_arenaTeamName(arenaTeamName)
    {
    }

    bool Execute() override
    {
        Player* leader = ObjectAccessor::FindPlayer(m_leaderGuid);
        if (!leader)
        {
            LOG_ERROR("playerbots", "ArenaGroupFormationOperation: Leader not found");
            return false;
        }

        // Step 1: Remove all members from their existing groups
        for (const ObjectGuid& memberGuid : m_memberGuids)
        {
            Player* member = ObjectAccessor::FindPlayer(memberGuid);
            if (!member)
                continue;

            Group* memberGroup = member->GetGroup();
            if (memberGroup)
            {
                memberGroup->RemoveMember(memberGuid);
                LOG_DEBUG("playerbots", "ArenaGroupFormationOperation: Removed {} from their existing group",
                         member->GetName());
            }
        }

        // Step 2: Disband leader's existing group
        Group* leaderGroup = leader->GetGroup();
        if (leaderGroup)
        {
            leaderGroup->Disband(true);
            LOG_DEBUG("playerbots", "ArenaGroupFormationOperation: Disbanded leader's existing group");
        }

        // Step 3: Create new group with leader
        Group* newGroup = new Group();
        if (!newGroup->Create(leader))
        {
            delete newGroup;
            LOG_ERROR("playerbots", "ArenaGroupFormationOperation: Failed to create arena group for leader {}",
                     leader->GetName());
            return false;
        }

        sGroupMgr->AddGroup(newGroup);
        LOG_DEBUG("playerbots", "ArenaGroupFormationOperation: Created new arena group with leader {}",
                 leader->GetName());

        // Step 4: Add members to the new group
        uint32 addedMembers = 0;
        for (const ObjectGuid& memberGuid : m_memberGuids)
        {
            Player* member = ObjectAccessor::FindPlayer(memberGuid);
            if (!member)
            {
                LOG_DEBUG("playerbots", "ArenaGroupFormationOperation: Member {} not found, skipping",
                         memberGuid.ToString());
                continue;
            }

            if (member->GetLevel() < 70)
            {
                LOG_DEBUG("playerbots", "ArenaGroupFormationOperation: Member {} is below level 70, skipping",
                         member->GetName());
                continue;
            }

            if (newGroup->AddMember(member))
            {
                addedMembers++;
                LOG_DEBUG("playerbots", "ArenaGroupFormationOperation: Added {} to arena group",
                         member->GetName());
            }
            else
                LOG_ERROR("playerbots", "ArenaGroupFormationOperation: Failed to add {} to arena group",
                         member->GetName());
        }

        if (addedMembers == 0)
        {
            LOG_ERROR("playerbots", "ArenaGroupFormationOperation: No members were added to the arena group");
            newGroup->Disband();
            return false;
        }

        // Step 5: Teleport members to leader and reset AI
        for (const ObjectGuid& memberGuid : m_memberGuids)
        {
            Player* member = ObjectAccessor::FindPlayer(memberGuid);
            if (!member || !newGroup->IsMember(memberGuid))
                continue;

            PlayerbotAI* memberBotAI = PlayerbotsMgr::instance().GetPlayerbotAI(member);
            if (memberBotAI)
                memberBotAI->Reset();

            member->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
            member->TeleportTo(leader->GetMapId(), leader->GetPositionX(), leader->GetPositionY(),
                              leader->GetPositionZ(), 0);

            LOG_DEBUG("playerbots", "ArenaGroupFormationOperation: Teleported {} to leader", member->GetName());
        }

        // Check if we have enough members
        if (newGroup->GetMembersCount() < m_requiredSize)
        {
            LOG_INFO("playerbots", "Team #{} <{}> Group is not ready for match (not enough members: {}/{})",
                    m_arenaTeamId, m_arenaTeamName, newGroup->GetMembersCount(), m_requiredSize);
            newGroup->Disband();
            return false;
        }

        LOG_INFO("playerbots", "Team #{} <{}> Group is ready for match with {} members",
                m_arenaTeamId, m_arenaTeamName, newGroup->GetMembersCount());
        return true;
    }

    ObjectGuid GetBotGuid() const override { return m_leaderGuid; }

    uint32 GetPriority() const override { return 60; }  // Very high priority (arena/BG operations)

    std::string GetName() const override { return "ArenaGroupFormation"; }

    bool IsValid() const override
    {
        Player* leader = ObjectAccessor::FindPlayer(m_leaderGuid);
        return leader != nullptr;
    }

private:
    ObjectGuid m_leaderGuid;
    std::vector<ObjectGuid> m_memberGuids;
    uint32 m_requiredSize;
    uint32 m_arenaTeamId;
    std::string m_arenaTeamName;
};

// Bot logout group cleanup operation
class BotLogoutGroupCleanupOperation : public PlayerbotOperation
{
public:
    BotLogoutGroupCleanupOperation(ObjectGuid botGuid) : m_botGuid(botGuid) {}

    bool Execute() override
    {
        Player* bot = ObjectAccessor::FindPlayer(m_botGuid);
        if (!bot)
            return false;

        PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!botAI)
            return false;

        Group* group = bot->GetGroup();
        if (group && !bot->InBattleground() && !bot->InBattlegroundQueue() && botAI->HasActivePlayerMaster())
            PlayerbotRepository::instance().Save(botAI);

        return true;
    }

    ObjectGuid GetBotGuid() const override { return m_botGuid; }
    uint32 GetPriority() const override { return 70; }
    std::string GetName() const override { return "BotLogoutGroupCleanup"; }

    bool IsValid() const override
    {
        Player* bot = ObjectAccessor::FindPlayer(m_botGuid);
        return bot != nullptr;
    }

private:
    ObjectGuid m_botGuid;
};

// Add player bot operation (for logging in bots from map threads)
class AddPlayerBotOperation : public PlayerbotOperation
{
public:
    AddPlayerBotOperation(ObjectGuid botGuid, uint32 masterAccountId)
        : m_botGuid(botGuid), m_masterAccountId(masterAccountId)
    {
    }

    bool Execute() override
    {
        sRandomPlayerbotMgr.AddPlayerBot(m_botGuid, m_masterAccountId);
        return true;
    }

    ObjectGuid GetBotGuid() const override { return m_botGuid; }

    uint32 GetPriority() const override { return 50; }  // High priority

    std::string GetName() const override { return "AddPlayerBot"; }

    bool IsValid() const override
    {
        return !ObjectAccessor::FindConnectedPlayer(m_botGuid);
    }

private:
    ObjectGuid m_botGuid;
    uint32 m_masterAccountId;
};

class OnBotLoginOperation : public PlayerbotOperation
{
public:
    OnBotLoginOperation(ObjectGuid botGuid, uint32 masterAccountId)
        : m_botGuid(botGuid), m_masterAccountId(masterAccountId)
    {
    }

    bool Execute() override
    {
        // find and verify bot still exists
        Player* bot = ObjectAccessor::FindConnectedPlayer(m_botGuid);
        if (!bot)
            return false;

        PlayerbotHolder* holder = &RandomPlayerbotMgr::instance();
        if (m_masterAccountId)
        {
            WorldSession* masterSession = sWorldSessionMgr->FindSession(m_masterAccountId);
            Player* masterPlayer = masterSession ? masterSession->GetPlayer() : nullptr;
            if (masterPlayer)
                holder = PlayerbotsMgr::instance().GetPlayerbotMgr(masterPlayer);
        }

        if (!holder)
            return false;

        holder->OnBotLogin(bot);
        return true;
    }

    ObjectGuid GetBotGuid() const override { return m_botGuid; }
    uint32 GetPriority() const override { return 100; }
    std::string GetName() const override { return "OnBotLogin"; }

    bool IsValid() const override { return ObjectAccessor::FindConnectedPlayer(m_botGuid) != nullptr; }

private:
    ObjectGuid m_botGuid;
    uint32 m_masterAccountId = 0;
};

class AuctionBuyOperation : public PlayerbotOperation
{
public:
    AuctionBuyOperation(ObjectGuid botGuid, ObjectGuid auctioneerGuid,
        uint32 auctionId, uint32 buyout, uint32 itemTemplateId)
        : m_botGuid(botGuid), m_auctioneerGuid(auctioneerGuid),
          m_auctionId(auctionId), m_buyout(buyout),
          m_itemTemplateId(itemTemplateId)
    {
    }

    bool Execute() override
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(m_botGuid);
        if (!bot)
            return false;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
            return false;

        Creature* auctioneer =
            bot->GetNPCIfCanInteractWith(m_auctioneerGuid,
                UNIT_NPC_FLAG_AUCTIONEER);
        if (!auctioneer)
            return false;

        AuctionHouseObject* auctionHouse =
            sAuctionMgr->GetAuctionsMap(auctioneer->GetFaction());
        if (!auctionHouse || !auctionHouse->GetAuction(m_auctionId))
            return false;

        uint32 botMoney = bot->GetMoney();

        WorldPacket packet(CMSG_AUCTION_PLACE_BID);
        packet << m_auctioneerGuid;
        packet << m_auctionId;
        packet << m_buyout;

        bot->GetSession()->HandleAuctionPlaceBid(packet);

        if (botAI->HasCheat(BotCheatMask::gold))
            bot->SetMoney(botMoney);

        if (auctionHouse->GetAuction(m_auctionId))
        {
            LOG_DEBUG("playerbots",
                "{}: failed to buy auction {} via {} (buyout={})",
                bot->GetName(), m_auctionId,
                m_auctioneerGuid.ToString(), m_buyout);
            return false;
        }

        ItemTemplate const* boughtItemProto =
            sObjectMgr->GetItemTemplate(m_itemTemplateId);

        LOG_DEBUG("playerbots",
            "{}: bought {} from auction house via {} for {}",
            bot->GetName(), boughtItemProto ? boughtItemProto->Name1 :
            std::to_string(m_itemTemplateId),
            m_auctioneerGuid.ToString(), m_buyout);

        std::ostringstream out;
        out << "Buying from auction house "
            << (boughtItemProto ? ChatHelper::FormatItem(boughtItemProto) :
                std::to_string(m_itemTemplateId))
            << " for " << m_buyout;
        botAI->TellMaster(out.str());

        return true;
    }

    ObjectGuid GetBotGuid() const override { return m_botGuid; }
    uint32 GetPriority() const override { return 50; }
    std::string GetName() const override { return "AuctionBuy"; }

    bool IsValid() const override
    {
        return ObjectAccessor::FindConnectedPlayer(m_botGuid) != nullptr;
    }

private:
    ObjectGuid m_botGuid;
    ObjectGuid m_auctioneerGuid;
    uint32 m_auctionId;
    uint32 m_buyout;
    uint32 m_itemTemplateId;
};

class AuctionSellOperation : public PlayerbotOperation
{
public:
    AuctionSellOperation(ObjectGuid botGuid, ObjectGuid auctioneerGuid,
        ObjectGuid itemGuid, uint32 itemEntry, uint32 itemCount,
        uint32 startBid, uint32 buyout, uint32 etime)
        : m_botGuid(botGuid), m_auctioneerGuid(auctioneerGuid),
          m_itemGuid(itemGuid), m_itemEntry(itemEntry),
          m_itemCount(itemCount), m_startBid(startBid),
          m_buyout(buyout), m_etime(etime)
    {
    }

    bool Execute() override
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(m_botGuid);
        if (!bot)
            return false;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
            return false;

        Creature* auctioneer =
            bot->GetNPCIfCanInteractWith(m_auctioneerGuid,
                UNIT_NPC_FLAG_AUCTIONEER);
        if (!auctioneer)
            return false;

        Item* item = bot->GetItemByGuid(m_itemGuid);
        if (!item || item->GetCount() < m_itemCount)
            return false;

        ItemTemplate const* proto = item->GetTemplate();
        if (!proto)
            proto = sObjectMgr->GetItemTemplate(m_itemEntry);

        uint32 oldCount = bot->GetItemCount(m_itemEntry, true);
        uint32 botMoney = bot->GetMoney();

        WorldPacket packet(CMSG_AUCTION_SELL_ITEM);
        packet << m_auctioneerGuid;
        packet << uint32(1);
        packet << m_itemGuid;
        packet << m_itemCount;
        packet << m_startBid;
        packet << m_buyout;
        packet << m_etime;

        bot->GetSession()->HandleAuctionSellItem(packet);

        if (botAI->HasCheat(BotCheatMask::gold))
            bot->SetMoney(botMoney);

        if (bot->GetItemCount(m_itemEntry, true) >= oldCount)
        {
            LOG_DEBUG("playerbots",
                "{}: failed to post {} x{} to auction house via {} "
                "(bid={}, buyout={})",
                bot->GetName(), proto ? proto->Name1 :
                std::to_string(m_itemEntry),
                m_itemCount, m_auctioneerGuid.ToString(), m_startBid,
                m_buyout);
            return false;
        }

        LOG_DEBUG("playerbots",
            "{}: posted {} x{} to auction house via {} (bid={}, buyout={})",
            bot->GetName(), proto ? proto->Name1 :
            std::to_string(m_itemEntry),
            m_itemCount, m_auctioneerGuid.ToString(), m_startBid,
            m_buyout);

        std::ostringstream out;
        out << "Posting to auction house "
            << (proto ? ChatHelper::FormatItem(proto, m_itemCount) :
                std::to_string(m_itemEntry))
            << " for " << m_startBid << ".." << m_buyout;
        botAI->TellMaster(out);

        return true;
    }

    ObjectGuid GetBotGuid() const override { return m_botGuid; }
    uint32 GetPriority() const override { return 50; }
    std::string GetName() const override { return "AuctionSell"; }

    bool IsValid() const override
    {
        return ObjectAccessor::FindConnectedPlayer(m_botGuid) != nullptr;
    }

private:
    ObjectGuid m_botGuid;
    ObjectGuid m_auctioneerGuid;
    ObjectGuid m_itemGuid;
    uint32 m_itemEntry;
    uint32 m_itemCount;
    uint32 m_startBid;
    uint32 m_buyout;
    uint32 m_etime;
};

#endif
