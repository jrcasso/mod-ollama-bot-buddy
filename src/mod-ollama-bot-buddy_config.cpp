#include "mod-ollama-bot-buddy_config.h"
#include "mod-ollama-bot-buddy_telemetry.h"
#include "Config.h"

bool g_EnableOllamaBotControl = true;
std::string g_OllamaBotControlUrl = "http://localhost:11434/api/generate";
std::string g_OllamaBotControlModel = "llama3.2:1b";
bool g_EnableOllamaBotBuddyDebug = false;

bool        g_EnableOllamaTelemetry       = true;
bool        g_OllamaTelemetryAllBots      = false;
bool        g_OllamaTelemetryFullPrompts  = false;
uint32_t    g_OllamaTelemetryPlayerRadius = 120;
std::string g_OllamaTelemetryDir          = "/azerothcore/env/dist/logs/telemetry";
bool g_EnableBotBuddyAddon = false;
std::string g_OllamaBotNames = "Ollamatest";
bool g_OllamaControlParty = true;
uint32 g_OllamaDecisionInterval = 3;
bool g_OllamaRequirePlayerOnline = true;
uint32 g_OllamaTestBotCount = 0;
std::string g_OllamaTestBotFilter = "any";
bool g_OllamaClearNonCombat = true;
bool g_OllamaDeterministicActions = true;
bool g_OllamaCombatStrafe = true;
uint32 g_OllamaSayCooldownSeconds = 240;
uint32 g_OllamaDuelCooldownSeconds = 3600;

OllamaBotControlConfigWorldScript::OllamaBotControlConfigWorldScript() : WorldScript("OllamaBotControlConfigWorldScript") {}

void OllamaBotControlConfigWorldScript::OnStartup()
{
    g_EnableOllamaBotControl = sConfigMgr->GetOption<bool>("OllamaBotControl.Enable", true);
    g_OllamaBotControlUrl = sConfigMgr->GetOption<std::string>("OllamaBotControl.Url", "http://localhost:11434/api/generate");
    g_OllamaBotControlModel = sConfigMgr->GetOption<std::string>("OllamaBotControl.Model", "llama3.2:1b");
    g_EnableOllamaBotBuddyDebug = sConfigMgr->GetOption<bool>("OllamaBotControl.Debug", false);

    g_EnableOllamaTelemetry       = sConfigMgr->GetOption<bool>("OllamaBotControl.Telemetry", true);
    g_OllamaTelemetryAllBots      = sConfigMgr->GetOption<bool>("OllamaBotControl.TelemetryAllBots", false);
    g_OllamaTelemetryFullPrompts  = sConfigMgr->GetOption<bool>("OllamaBotControl.TelemetryFullPrompts", false);
    g_OllamaTelemetryPlayerRadius = sConfigMgr->GetOption<uint32_t>("OllamaBotControl.TelemetryPlayerRadius", 120);
    g_OllamaTelemetryDir          = sConfigMgr->GetOption<std::string>("OllamaBotControl.TelemetryDir",
                                        "/azerothcore/env/dist/logs/telemetry");
    g_EnableBotBuddyAddon = sConfigMgr->GetOption<bool>("OllamaBotControl.EnableBotBuddyAddon", false);
    g_OllamaBotNames = sConfigMgr->GetOption<std::string>("OllamaBotControl.BotNames", "Ollamatest");
    g_OllamaControlParty = sConfigMgr->GetOption<bool>("OllamaBotControl.ControlPartyBots", true);
    g_OllamaDecisionInterval = sConfigMgr->GetOption<uint32>("OllamaBotControl.DecisionIntervalSeconds", 3);
    g_OllamaRequirePlayerOnline = sConfigMgr->GetOption<bool>("OllamaBotControl.RequirePlayerOnline", true);
    g_OllamaTestBotCount = sConfigMgr->GetOption<uint32>("OllamaBotControl.TestBotCount", 0);
    g_OllamaTestBotFilter = sConfigMgr->GetOption<std::string>("OllamaBotControl.TestBotFilter", "any");
    g_OllamaClearNonCombat = sConfigMgr->GetOption<bool>("OllamaBotControl.ClearNonCombatStrategies", true);
    g_OllamaDeterministicActions = sConfigMgr->GetOption<bool>("OllamaBotControl.DeterministicActions", true);
    g_OllamaCombatStrafe = sConfigMgr->GetOption<bool>("OllamaBotControl.CombatStrafe", true);
    g_OllamaSayCooldownSeconds = sConfigMgr->GetOption<uint32>("OllamaBotControl.SayCooldownSeconds", 240);
    g_OllamaDuelCooldownSeconds = sConfigMgr->GetOption<uint32>("OllamaBotControl.DuelCooldownSeconds", 3600);


    // Everything above has been read, so the writer can resolve its directory
    // and open this run's file.
    Telemetry_Start();
}

void OllamaBotControlConfigWorldScript::OnShutdown()
{
    // Drains the queue and closes the file. Without this the tail of the last
    // session -- which is the part worth reading after a crash or a bad run --
    // is whatever happened to have been flushed already.
    Telemetry_Stop();
}
