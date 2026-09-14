#include "mod-ollama-bot-buddy_config.h"
#include "Config.h"

bool g_EnableOllamaBotControl = true;
std::string g_OllamaBotControlUrl = "http://localhost:11434/api/generate";
std::string g_OllamaBotControlModel = "llama3.2:1b";
bool g_EnableOllamaBotBuddyDebug = false;
bool g_EnableBotBuddyAddon = false;
std::string g_OllamaBotNames = "Ollamatest";
bool g_OllamaControlParty = true;
uint32 g_OllamaDecisionInterval = 3;
bool g_OllamaRequirePlayerOnline = true;
uint32 g_OllamaTestBotCount = 0;
std::string g_OllamaTestBotFilter = "any";

OllamaBotControlConfigWorldScript::OllamaBotControlConfigWorldScript() : WorldScript("OllamaBotControlConfigWorldScript") {}

void OllamaBotControlConfigWorldScript::OnStartup()
{
    g_EnableOllamaBotControl = sConfigMgr->GetOption<bool>("OllamaBotControl.Enable", true);
    g_OllamaBotControlUrl = sConfigMgr->GetOption<std::string>("OllamaBotControl.Url", "http://localhost:11434/api/generate");
    g_OllamaBotControlModel = sConfigMgr->GetOption<std::string>("OllamaBotControl.Model", "llama3.2:1b");
    g_EnableOllamaBotBuddyDebug = sConfigMgr->GetOption<bool>("OllamaBotControl.Debug", false);
    g_EnableBotBuddyAddon = sConfigMgr->GetOption<bool>("OllamaBotControl.EnableBotBuddyAddon", false);
    g_OllamaBotNames = sConfigMgr->GetOption<std::string>("OllamaBotControl.BotNames", "Ollamatest");
    g_OllamaControlParty = sConfigMgr->GetOption<bool>("OllamaBotControl.ControlPartyBots", true);
    g_OllamaDecisionInterval = sConfigMgr->GetOption<uint32>("OllamaBotControl.DecisionIntervalSeconds", 3);
    g_OllamaRequirePlayerOnline = sConfigMgr->GetOption<bool>("OllamaBotControl.RequirePlayerOnline", true);
    g_OllamaTestBotCount = sConfigMgr->GetOption<uint32>("OllamaBotControl.TestBotCount", 0);
    g_OllamaTestBotFilter = sConfigMgr->GetOption<std::string>("OllamaBotControl.TestBotFilter", "any");
}
