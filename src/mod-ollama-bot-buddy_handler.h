#include "ScriptMgr.h"
#include <string>
#include <Group.h>
#include <Channel.h>

extern std::unordered_map<uint64_t, std::deque<std::pair<std::string, std::string>>> botPlayerMessages;
extern std::mutex botPlayerMessagesMutex;

class BotBuddyChatHandler : public PlayerScript
{
public:
    BotBuddyChatHandler() : PlayerScript("BotBuddyChatHandler") {}

    // AzerothCore replaced the observational OnPlayerChat hooks with the
    // bool-returning OnPlayerCanUseChat family. We only observe chat, so these
    // always allow the message through.
    using PlayerScript::OnPlayerCanUseChat;

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg) override;
    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Group* group) override;
    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Channel* channel) override;

private:
    void ProcessChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Channel* channel = nullptr);
};
