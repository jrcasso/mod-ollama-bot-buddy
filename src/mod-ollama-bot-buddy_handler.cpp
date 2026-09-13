#include "mod-ollama-bot-buddy_handler.h"
#include "Log.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "ObjectAccessor.h"
#include <mutex>
#include <algorithm>
#include <unordered_map>
#include <deque>
#include <chrono>

// Stores the last messages: [bot GUID][playerName] => pair<text, timestamp>
std::unordered_map<uint64_t, std::deque<std::pair<std::string, std::string>>> botPlayerMessages;
std::mutex botPlayerMessagesMutex;

bool BotBuddyChatHandler::OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg)
{
    ProcessChat(player, type, lang, msg, nullptr);
    return true;
}
bool BotBuddyChatHandler::OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Group* /*group*/)
{
    ProcessChat(player, type, lang, msg, nullptr);
    return true;
}
bool BotBuddyChatHandler::OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Channel* channel)
{
    ProcessChat(player, type, lang, msg, channel);
    return true;
}

void BotBuddyChatHandler::ProcessChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Channel* channel)
{
    LOG_INFO("server.loading", "ProcessChat: sender={} type={} lang={} msg='{}' channel={}", 
    player ? player->GetName() : "NULL", type, lang, msg, channel ? channel->GetName() : "nullptr");

    if (!player || msg.empty()) return;
    PlayerbotAI* senderAI = PlayerbotsMgr::instance().GetPlayerbotAI(player);
    if (senderAI && senderAI->IsBotAI()) return;

    std::lock_guard<std::mutex> lock(botPlayerMessagesMutex);

    // Party/raid chat addresses the whole group.
    //
    // Previously a message only reached a bot if you typed that bot's name. In a
    // dungeon that means naming five randomly-generated bots individually, which
    // nobody is going to do mid-pull. When you speak in party or raid chat, every
    // bot in your group hears you.
    //
    // This also avoids scanning all ~500 world bots for these messages: we walk
    // the group's members instead.
    bool const isGroupChat = (type == CHAT_MSG_PARTY || type == CHAT_MSG_PARTY_LEADER ||
                              type == CHAT_MSG_RAID  || type == CHAT_MSG_RAID_LEADER ||
                              type == CHAT_MSG_RAID_WARNING);

    if (isGroupChat)
    {
        if (Group* group = player->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == player || !member->IsAlive()) continue;

                PlayerbotAI* memberAI = PlayerbotsMgr::instance().GetPlayerbotAI(member);
                if (!memberAI || !memberAI->IsBotAI()) continue;

                botPlayerMessages[member->GetGUID().GetRawValue()].emplace_back(player->GetName(), msg);
            }
            return;
        }
    }

    auto const& allPlayers = ObjectAccessor::GetPlayers();
    for (auto const& itr : allPlayers)
    {
        Player* bot = itr.second;
        if (!bot || !bot->IsAlive()) continue;

        PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!botAI || !botAI->IsBotAI()) continue;

        std::string messageLower = msg;
        std::string botNameLower = bot->GetName();
        std::transform(messageLower.begin(), messageLower.end(), messageLower.begin(), ::tolower);
        std::transform(botNameLower.begin(), botNameLower.end(), botNameLower.begin(), ::tolower);

        // If the player mentions the bot in the message
        if (messageLower.find(botNameLower) != std::string::npos)
        {
            botPlayerMessages[bot->GetGUID().GetRawValue()].emplace_back(player->GetName(), msg);
        }
    }
}