#pragma once
#include "ScriptMgr.h"
#include <string>

extern bool g_EnableOllamaBotControl;
extern std::string g_OllamaBotControlUrl;
extern std::string g_OllamaBotControlModel;
extern bool g_EnableOllamaBotBuddyDebug;
extern bool g_EnableBotBuddyAddon;
// Which bots the LLM drives. Previously a single hardcoded name, which made
// group content impossible: you cannot run a dungeon with one AI party member.
extern std::string g_OllamaBotNames;      // comma-separated explicit list
extern bool        g_OllamaControlParty;  // drive any bot grouped with a real player
// Minimum seconds between LLM decisions for a single bot. Without this the loop
// re-queries the instant the previous reply lands, so a 5-bot dungeon party
// saturates the GPU and every bot's decisions get slower.
extern uint32      g_OllamaDecisionInterval;
// Gate LLM work on a real player being online. On by default: with nobody
// playing there is no one to watch the bots, and the model stays resident in
// memory for nothing. Turn off to exercise the loop without logging in.
extern bool        g_OllamaRequirePlayerOnline;
// Drive this many arbitrary online bots, regardless of name or grouping.
// Testing only: BotNames silently controls nothing when the named bot has been
// rotated out of the world by the level bracket system, which is easy to do and
// looks exactly like the module being broken.
extern uint32      g_OllamaTestBotCount;

class OllamaBotControlConfigWorldScript : public WorldScript
{
public:
    OllamaBotControlConfigWorldScript();
    void OnStartup() override;
};
