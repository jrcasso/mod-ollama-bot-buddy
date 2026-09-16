#pragma once
#include "ScriptMgr.h"
#include <string>

extern bool g_EnableOllamaBotControl;
extern std::string g_OllamaBotControlUrl;
extern std::string g_OllamaBotControlModel;
extern bool g_EnableOllamaBotBuddyDebug;

// Gameplay telemetry -- see mod-ollama-bot-buddy_telemetry.h.
extern bool        g_EnableOllamaTelemetry;
extern bool        g_OllamaTelemetryAllBots;
extern bool        g_OllamaTelemetryFullPrompts;
extern uint32_t    g_OllamaTelemetryPlayerRadius;
extern std::string g_OllamaTelemetryDir;
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
// "any" adopts whichever bots come first, which in a 500-bot world means bots
// idling in towns: only 4% of captured prompts had the bot in combat, so combat
// decisions were barely sampled. "combat" restricts adoption to bots actually
// fighting (measured: 4% -> 90%) and releases the slot when they stop, so the
// sample keeps tracking live combat.
extern std::string g_OllamaTestBotFilter;

// Whether takeover deletes the bot's non-combat brain.
//
// That brain is mod-playerbots' deterministic AI for questing, travel, looting
// and vendoring, and it demonstrably works: 5,724 quest turn-ins across the 490
// bots this module does not control. Clearing it is what stops the native AI
// fighting the LLM over movement, but it is also why a taken-over bot cannot
// quest, loot or travel on its own.
//
// Default 1 preserves the existing behaviour. Set to 0 to measure what the
// deterministic AI was contributing.
extern bool g_OllamaClearNonCombat;

// Execute the situation assessment directly instead of asking the model to agree
// with it. The assessment already determines the correct command for a corpse
// underfoot, an attacker on you, a giver in reach or a visible objective; putting
// that through a 24,000 character prompt costs ~21s and was obeyed far from
// always. Inference is then reserved for ticks with no definite answer, which is
// where speech and genuinely open-ended choices live.
extern bool g_OllamaDeterministicActions;

// Lateral repositioning during combat. Bots sit on IDLE_MOTION_TYPE for 88.6% of
// in-combat observations, i.e. they fight standing still. Deterministic: range
// band, line of sight and cast state are all known server-side.
extern bool g_OllamaCombatStrafe;

// Minimum seconds between two things a bot says out loud, jittered so a crowd
// does not speak in lockstep. Measured before adding this: 88 lines from 89
// decisions, i.e. a bot narrated essentially every action it took. When to open
// your mouth is a cooldown, not a judgement, so it lives in code.
extern uint32 g_OllamaSayCooldownSeconds;

// Minimum seconds between one bot challenging another to a duel, jittered to
// double. 0 disables. Duelling outside a city is one of the most recognisable
// things on a populated server, and the core already refuses duels inside
// sanctuaries, so this naturally happens at the gates rather than in the bank.
extern uint32 g_OllamaDuelCooldownSeconds;

class OllamaBotControlConfigWorldScript : public WorldScript
{
public:
    OllamaBotControlConfigWorldScript();
    void OnStartup() override;
    void OnShutdown() override;
};
