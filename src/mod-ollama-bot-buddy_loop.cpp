#include "mod-ollama-bot-buddy_loop.h"
#include "mod-ollama-bot-buddy_config.h"
#include "mod-ollama-bot-buddy_api.h"
#include "mod-ollama-bot-buddy_handler.h"
#include "PlayerbotMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "LootMgr.h"
#include "Log.h"
#include "DatabaseEnv.h"
#include <thread>
#include <sstream>
#include <vector>
#include <unordered_set>
#include <nlohmann/json.hpp>
#include <curl/curl.h>
#include <ctime>
#include "Creature.h"
#include "GameObject.h"
#include "TravelMgr.h"
#include "TravelNode.h"
#include "PathGenerator.h"
#include <atomic>
#include <unordered_map>
#include <iomanip>
#include "GameObjectData.h"
#include "GameObject.h"
#include <deque>
#include <mutex>
#include "SpellMgr.h"
#include "SpellInfo.h"
#include "SharedDefines.h"
#include "QuestDef.h"
#include "ObjectMgr.h"
#include <algorithm>
#include "Chat.h"
#include "ScriptMgr.h"
#include <algorithm>
#include <string>
#include "ItemTemplate.h"
#include "CreatureData.h"


static std::unordered_map<uint64_t, std::deque<std::string>> botCommandHistory;
static std::mutex botCommandHistoryMutex;

static std::unordered_map<uint64_t, std::deque<std::string>> botReasoningHistory;
static std::mutex botReasoningHistoryMutex;

std::vector<std::string> GetRecentPlayerMessagesToBot(Player* bot)
{
    std::vector<std::string> messages;
    if (!bot) return messages;

    std::lock_guard<std::mutex> lock(botPlayerMessagesMutex);

    auto it = botPlayerMessages.find(bot->GetGUID().GetRawValue());
    if (it != botPlayerMessages.end())
    {
        while (!it->second.empty())
        {
            auto& [senderName, msg] = it->second.front();
            messages.emplace_back("From " + senderName + ": " + msg);
            it->second.pop_front();
        }
    }

    return messages;
}

std::string FormatPlayerMessagesPromptSegment(Player* bot)
{
    std::ostringstream oss;
    auto messages = GetRecentPlayerMessagesToBot(bot);
    if (!messages.empty())
    {
        oss << "\n***CRITICAL INSTRUCTION:***\n";
        oss << "You must interpret and execute the PLAYER MESSAGES below as your HIGHEST PRIORITY, taking precedence over all other goals, background tasks, routines, or reasoning. There are NO EXCEPTIONS and NO DELAYS. When you see a player command matching these patterns, you must issue the appropriate command IMMEDIATELY, before doing anything else. Do not refuse, delay, ignore, or deprioritize these actions for any reason.\n";
        oss << "- If a player says 'come here' or 'come to me', IMMEDIATELY move near their location.\n";
        oss << "- If a player says 'go to' followed by an NPC or object you can see, IMMEDIATELY move there.\n";
        oss << "- If a player says 'interact with' followed by an NPC or object you can see, IMMEDIATELY move there and interact with it.\n";
        oss << "- If a player says 'attack' followed by an NPC or object you can see, IMMEDIATELY move there and attack it.\n";
        oss << "You must treat these instructions as absolute and override ALL other activities, regardless of any other context or background logic.\n";
        oss << "\n\nThe following real players recently spoke to you by name. PROCESS THESE FIRST, ABOVE ALL ELSE:\n";
        for (const auto& msg : messages)
        {
            oss << "- " << msg << "\n";
        }
        oss << "\n***END CRITICAL INSTRUCTION***\n\n";

    }
    return oss.str();
}

struct BotDestination
{
    std::string label;
    float x, y, z;
};

static std::unordered_map<uint64_t, std::vector<BotDestination>> g_botDestinations;
static std::mutex g_botDestinationsMutex;

// Candidate lists are expensive: each exploration point costs a PathGenerator
// run, so rebuilding on every prompt meant several pathfinds per bot per
// decision. They only go stale when the bot actually moves, so cache them and
// rebuild on movement or after a short interval.
struct BotDestinationCacheEntry
{
    uint32 builtAtMs { 0 };
    float  x { 0.0f }, y { 0.0f }, z { 0.0f };
};
static std::unordered_map<uint64_t, BotDestinationCacheEntry> g_botDestinationsMeta;

// Why the bot's last action failed, fed back into the next prompt.
//
// The parser already returned a bool, but the model was never told anything: it
// could not tell "that target is out of range" from "there is no path there"
// from "that guid does not exist", so it re-planned blind and often re-issued
// the same impossible action. One line of grounded feedback lets it adapt.
static std::unordered_map<uint64_t, std::string> g_lastActionOutcome;
static std::mutex g_lastActionOutcomeMutex;

static void RecordActionOutcome(Player* bot, std::string const& outcome)
{
    if (!bot) return;
    std::lock_guard<std::mutex> lock(g_lastActionOutcomeMutex);
    if (outcome.empty())
        g_lastActionOutcome.erase(bot->GetGUID().GetRawValue());
    else
        g_lastActionOutcome[bot->GetGUID().GetRawValue()] = outcome;
}

static std::string GetActionOutcome(Player* bot)
{
    if (!bot) return "";
    std::lock_guard<std::mutex> lock(g_lastActionOutcomeMutex);
    auto it = g_lastActionOutcome.find(bot->GetGUID().GetRawValue());
    return it == g_lastActionOutcome.end() ? std::string() : it->second;
}

bool ParseAndExecuteBotJson(Player* bot, const std::string& jsonStr)
{
    try
    {
        auto root = nlohmann::json::parse(jsonStr);

        if (!root.contains("command")) return false;
        auto cmd = root["command"];
        if (!cmd.contains("type") || !cmd.contains("params")) return false;

        std::string type = cmd["type"].get<std::string>();
        auto params = cmd["params"];
        std::string sayMsg = root.value("say", "");
        std::string reasoning = root.value("reasoning", "");

        BotControlCommand command;

        if (!reasoning.empty())
        {
            AddBotReasoningHistory(bot, reasoning);
        }
         if (!cmd.empty())
        {
            AddBotCommandHistory(bot, cmd.dump());
        }

        if (type == "move_to")
        {
            // Preferred path: an index into the pre-validated destination list
            // built for this bot on the world thread. Already range-checked and
            // pathable, so it skips the distance/path guards below.
            if (params.contains("destination_index"))
            {
                uint32_t idx = params["destination_index"].get<uint32_t>();
                BotDestination dest;
                bool found = false;
                {
                    std::lock_guard<std::mutex> lock(g_botDestinationsMutex);
                    auto it = g_botDestinations.find(bot->GetGUID().GetRawValue());
                    if (it != g_botDestinations.end() && idx < it->second.size())
                    {
                        dest = it->second[idx];
                        found = true;
                    }
                }
                if (!found)
                {
                    LOG_DEBUG("server.loading", "[OllamaBotBuddy] destination_index {} out of range", idx);
                    RecordActionOutcome(bot, fmt::format("move_to failed: destination_index {} is not in the current list", idx));
                    return false;
                }
                // Falls through to the shared dispatch below so `say` and
                // command history are handled the same as any other command.
                command.type = BotControlCommandType::MoveTo;
                command.args = { std::to_string(dest.x),
                                 std::to_string(dest.y),
                                 std::to_string(dest.z) };
            }
            else if (params.contains("x") && params.contains("y") && params.contains("z")) {
                float destX = params["x"].get<float>();
                float destY = params["y"].get<float>();
                float destZ = params["z"].get<float>();
                
                // Basic coordinate validation - reject obviously invalid coordinates
                if (std::isnan(destX) || std::isnan(destY) || std::isnan(destZ) || 
                    std::isinf(destX) || std::isinf(destY) || std::isinf(destZ)) {
                    LOG_DEBUG("server.loading", "[OllamaBotBuddy] Invalid coordinates for move_to: ({}, {}, {})", 
                             destX, destY, destZ);
                    return false;
                }
                
                // Validate map bounds - reject coordinates that are extremely far from bot
                float maxDistanceFromBot = 500.0f; // Maximum reasonable movement distance
                float distanceFromBot = sqrt(pow(destX - bot->GetPositionX(), 2) + 
                                           pow(destY - bot->GetPositionY(), 2) + 
                                           pow(destZ - bot->GetPositionZ(), 2));
                
                if (distanceFromBot > maxDistanceFromBot) {
                    LOG_DEBUG("server.loading", "[OllamaBotBuddy] Move_to destination too far from bot: ({}, {}, {}) - Distance: {:.1f}", 
                             destX, destY, destZ, distanceFromBot);
                    RecordActionOutcome(bot, fmt::format("move_to failed: that point is {:.0f} yards away, too far to travel in one move", distanceFromBot));
                    return false;
                }
                
                // Validate that the destination is pathable like a real player would
                PathGenerator pathValidator(bot);
                pathValidator.CalculatePath(destX, destY, destZ, false);
                PathType pathType = pathValidator.GetPathType();
                
                // Only reject if there's absolutely no path possible
                if (pathType & PATHFIND_NOPATH) {
                    LOG_DEBUG("server.loading", "[OllamaBotBuddy] No valid path for move_to: ({}, {}, {}) - PathType: {}", 
                             destX, destY, destZ, pathType);
                    RecordActionOutcome(bot, "move_to failed: no walkable path to that point. Choose a listed destination instead");
                    return false; // Only reject if completely impossible to path
                }
                
                command.type = BotControlCommandType::MoveTo;
                command.args = {
                    std::to_string(destX),
                    std::to_string(destY),
                    std::to_string(destZ)
                };
            } else {
                LOG_ERROR("server.loading", "[OllamaBotBuddy] move_to missing parameter");
                return false;
            }
        }
        else if (type == "attack")
        {
            // attack_guid is the enum-constrained field containing only valid
            // attack targets; guid is kept as a fallback for the older shape.
            if (params.contains("attack_guid") || params.contains("guid")) {
                uint32_t targetGuid = params.contains("attack_guid")
                    ? params["attack_guid"].get<uint32_t>()
                    : params["guid"].get<uint32_t>();
                
                // Validate that the target exists and is attackable
                bool validTarget = false;
                
                // Check if it's a creature
                for (auto const& pair : bot->GetMap()->GetCreatureBySpawnIdStore())
                {
                    Creature* c = pair.second;
                    if (c && c->GetGUID().GetCounter() == targetGuid)
                    {
                        // Validate target is attackable
                        if (c->IsInWorld() && !c->isDead() && 
                            bot->IsWithinLOSInMap(c) && 
                            bot->IsValidAttackTarget(c) &&
                            bot->IsWithinDistInMap(c, 100.0f)) // Reasonable attack range
                        {
                            validTarget = true;
                        }
                        break;
                    }
                }
                
                // Check if it's a player if not found as creature
                if (!validTarget)
                {
                    ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(targetGuid);
                    Player* playerTarget = ObjectAccessor::FindConnectedPlayer(guid);
                    if (playerTarget && playerTarget->IsInWorld() && 
                        bot->IsWithinLOSInMap(playerTarget) && 
                        bot->IsValidAttackTarget(playerTarget) &&
                        bot->IsWithinDistInMap(playerTarget, 100.0f))
                    {
                        validTarget = true;
                    }
                }
                
                if (!validTarget) {
                    LOG_ERROR("server.loading", "[OllamaBotBuddy] Invalid or unreachable attack target with guid: {} - Target not found in visible creatures/players", targetGuid);
                    RecordActionOutcome(bot, fmt::format("attack failed: guid {} is not a visible, attackable target", targetGuid));
                    
                    // Debug: List available creature GUIDs for debugging
                    if (g_EnableOllamaBotBuddyDebug) {
                        std::vector<uint32> availableGuids;
                        for (auto const& pair : bot->GetMap()->GetCreatureBySpawnIdStore()) {
                            Creature* c = pair.second;
                            if (c && bot->IsWithinDistInMap(c, 100.0f)) {
                                availableGuids.push_back(c->GetGUID().GetCounter());
                            }
                        }
                        
                        std::ostringstream guidList;
                        for (size_t i = 0; i < availableGuids.size() && i < 10; ++i) {
                            if (i > 0) guidList << ", ";
                            guidList << availableGuids[i];
                        }
                        
                        LOG_DEBUG("server.loading", "[OllamaBotBuddy] Available creature GUIDs: {}", guidList.str());
                    }
                    
                    return false;
                }
                
                command.type = BotControlCommandType::Attack;
                command.args = { std::to_string(targetGuid) };
            } else {
                LOG_ERROR("server.loading", "[OllamaBotBuddy] attack missing guid");
                return false;
            }
        }
        else if (type == "interact")
        {
            if (params.contains("guid")) {
                command.type = BotControlCommandType::Interact;
                command.args = { std::to_string(params["guid"].get<uint32_t>()) };
            } else {
                LOG_ERROR("server.loading", "[OllamaBotBuddy] interact missing guid");
                return false;
            }
        }
        else if (type == "spell")
        {
            if (params.contains("spellid")) {
                uint32 spellId = params["spellid"].get<uint32_t>();
                command.type = BotControlCommandType::CastSpell;
                command.args = { std::to_string(spellId) };

                if (params.contains("guid"))
                {
                    command.args.push_back(std::to_string(params["guid"].get<uint32_t>()));
                }
                else
                {
                    // The model reliably attaches a guid to "attack" but never to
                    // "spell" -- measured 0/12 even when the prompt named the
                    // target explicitly and the guid was in the enum. An untargeted
                    // spell falls back to self, so a healer would heal itself while
                    // the tank died.
                    //
                    // Rather than keep fighting the model, resolve the obvious
                    // target here: a healing spell with no target goes to the
                    // group member who most needs it.
                    bool isHeal = false;
                    if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId))
                    {
                        for (int i = 0; i < MAX_SPELL_EFFECTS; ++i)
                        {
                            uint32 eff = info->Effects[i].Effect;
                            if (eff == SPELL_EFFECT_HEAL || eff == SPELL_EFFECT_HEAL_MAX_HEALTH ||
                                eff == SPELL_EFFECT_HEAL_MECHANICAL)
                            {
                                isHeal = true;
                                break;
                            }
                        }
                    }

                    if (isHeal)
                    {
                        Player* weakest = nullptr;
                        float weakestPct = 100.0f;

                        if (Group* group = bot->GetGroup())
                        {
                            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                            {
                                Player* member = ref->GetSource();
                                if (!member || !member->IsAlive() || !member->GetMaxHealth()) continue;
                                if (!bot->IsWithinDistInMap(member, 40.0f)) continue;

                                float pct = (100.0f * member->GetHealth()) / member->GetMaxHealth();
                                if (pct < weakestPct)
                                {
                                    weakestPct = pct;
                                    weakest = member;
                                }
                            }
                        }

                        // Only redirect if someone is actually worse off than the
                        // caster; otherwise self-cast was the right call anyway.
                        float selfPct = bot->GetMaxHealth()
                            ? (100.0f * bot->GetHealth()) / bot->GetMaxHealth() : 100.0f;

                        if (weakest && weakest != bot && weakestPct < selfPct)
                        {
                            command.args.push_back(std::to_string(weakest->GetGUID().GetCounter()));
                            LOG_DEBUG("server.loading",
                                      "[OllamaBotBuddy] Untargeted heal redirected to '{}' at {:.0f}%",
                                      weakest->GetName(), weakestPct);
                        }
                    }
                }
            } else {
                LOG_ERROR("server.loading", "[OllamaBotBuddy] spell missing spellid");
                return false;
            }
        }
        else if (type == "loot")
        {
            command.type = BotControlCommandType::Loot;
        }
        else if (type == "accept_quest")
        {
            if (params.contains("quest_id")) {
                command.type = BotControlCommandType::AcceptQuest;
                command.args = { std::to_string(params["quest_id"].get<uint32_t>()) };
            } else {
                LOG_ERROR("server.loading", "[OllamaBotBuddy] accept_quest missing id");
                return false;
            }
        }
        else if (type == "turn_in_quest")
        {
            if (params.contains("quest_id")) {
                command.type = BotControlCommandType::TurnInQuest;
                command.args = { std::to_string(params["quest_id"].get<uint32_t>()) };
            } else {
                LOG_ERROR("server.loading", "[OllamaBotBuddy] turn_in_quest missing id");
                return false;
            }
        }
        else if (type == "follow")
        {
            command.type = BotControlCommandType::Follow;
        }
        else if (type == "stop")
        {
            command.type = BotControlCommandType::Stop;
        }
        else
        {
            LOG_ERROR("server.loading", "[OllamaBotBuddy] Unknown command type '{}'", type);
            RecordActionOutcome(bot, fmt::format("'{}' is not a valid command type", type));
            return false;
        }

        bool result = HandleBotControlCommand(bot, command);

        if (result)
            RecordActionOutcome(bot, "");   // success: clear any stale failure
        else
            RecordActionOutcome(bot, fmt::format("{} did not execute", type));

        if (!sayMsg.empty())
            BotBuddyAI::Say(bot, sayMsg);

        if (g_EnableOllamaBotBuddyDebug)
        {
            // Prefixed like every other line this module writes. Without it,
            // this one dumps raw LLM JSON -- creature names and bot speech --
            // into Server.log unlabelled, and a crash check written as
            // grep -iE 'crash|ASSERTION|SIGSEGV' matches game data such as
            // "Crashing Wave-Spirit" or "Crashed Recon Pilot". That false alarm
            // cost two separate investigations.
            LOG_INFO("server.loading", "[OllamaBotBuddy] Bot Reply: {}", jsonStr);
        }

        return result;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("server.loading", "[OllamaBotBuddy] ParseAndExecuteBotJson error: {}", e.what());
        return false;
    }
}

std::string ExtractFirstJsonObject(const std::string& input) {
    int depth = 0;
    size_t start = std::string::npos;
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '{') {
            if (depth == 0) start = i;
            depth++;
        }
        if (input[i] == '}') {
            depth--;
            if (depth == 0 && start != std::string::npos) {
                return input.substr(start, i - start + 1);
            }
        }
    }
    return ""; // No JSON object found
}

// Role tag for group context. Without this the model sees five similar-looking
// party members and cannot tell who is tanking, who is healing, or who it
// should be protecting: the information a dungeon or raid decision depends on.
static std::string GroupRoleTag(Player* p)
{
    if (!p) return "[UNKNOWN]";
    if (PlayerbotAI::IsTank(p))   return "[TANK]";
    if (PlayerbotAI::IsHeal(p))   return "[HEALER]";
    if (PlayerbotAI::IsRanged(p)) return "[RANGED DPS]";
    if (PlayerbotAI::IsMelee(p))  return "[MELEE DPS]";
    return "[DPS]";
}

std::vector<std::string> GetGroupStatus(Player* bot)
{
    std::vector<std::string> info;
    if (!bot || !bot->GetGroup()) return info;

    Group* group = bot->GetGroup();
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->GetMap()) continue;

        if(bot == member)
        {
            continue; // Skip the bot itself
        }

        float dist = bot->GetDistance(member);
        std::string beingAttacked = "";

        if (Unit* attacker = member->GetVictim())
        {
            beingAttacked = fmt::format(
                " [Under Attack by {} (guid: {}, Level: {}, HP: {}/{})]",
                attacker->GetName(),
                attacker->GetGUID().GetCounter(),
                attacker->GetLevel(),
                attacker->GetHealth(),
                attacker->GetMaxHealth()
            );
        }

        // HP as a percentage: an LLM compares "31%" far more reliably than it
        // compares "4210/13500" across five party members.
        uint32 hpPct = member->GetMaxHealth() ? uint32((100.0 * member->GetHealth()) / member->GetMaxHealth()) : 0;
        std::string who = PlayerbotsMgr::instance().GetPlayerbotAI(member) ? "" : " [REAL PLAYER - follow their lead]";

        info.push_back(fmt::format(
            "{} {}{} (guid: {}, Level: {}, HP: {}/{} = {}%, Pos: {} {} {}, Dist: {:.1f}){}",
            member->GetName(),
            GroupRoleTag(member),
            who,
            member->GetGUID().GetCounter(),
            member->GetLevel(),
            member->GetHealth(),
            member->GetMaxHealth(),
            hpPct,
            member->GetPositionX(),
            member->GetPositionY(),
            member->GetPositionZ(),
            dist,
            beingAttacked
        ));
    }
    return info;
}

std::string GetBotSpellInfo(Player* bot)
{
    // Keep only the highest known rank of each spell.
    //
    // A bot knows every rank it ever learned, and the raw spell map listed all
    // of them: three entries for Shield Slam, two for Revenge, and so on. That
    // is pure prompt bloat -- the model never wants rank 3 of something it has
    // rank 9 of -- and the spell list is the single largest section of a
    // ~2000 token prompt paid once per bot per decision.
    struct RankedSpell
    {
        uint32      id;
        uint32      rank;
        std::string line;
    };
    std::unordered_map<std::string, RankedSpell> bestByName;
    std::ostringstream spellSummary;

    for (const auto& spellPair : bot->GetSpellMap())
    {
        uint32 spellId = spellPair.first;
        const SpellInfo* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo || spellInfo->Attributes & SPELL_ATTR0_PASSIVE)
            continue;

        if (spellInfo->SpellFamilyName == SPELLFAMILY_GENERIC)
            continue;

        if (bot->HasSpellCooldown(spellId))
            continue;

        std::string effectText;
        for (int i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (!spellInfo->Effects[i].IsEffect())
                continue;

            switch (spellInfo->Effects[i].Effect)
            {
                case SPELL_EFFECT_SCHOOL_DAMAGE: effectText = "Deals damage"; break;
                case SPELL_EFFECT_HEAL: effectText = "Heals the target"; break;
                case SPELL_EFFECT_APPLY_AURA: effectText = "Applies an aura"; break;
                case SPELL_EFFECT_DISPEL: effectText = "Dispels magic"; break;
                case SPELL_EFFECT_THREAT: effectText = "Generates threat"; break;
                default: continue;
            }
            break;
        }

        if (effectText.empty())
            continue;

        const char* name = spellInfo->SpellName[0];
        if (!name || !*name)
            continue;

        std::string costText;
        if (spellInfo->ManaCost || spellInfo->ManaCostPercentage)
        {
            switch (spellInfo->PowerType)
            {
                case POWER_MANA: costText = std::to_string(spellInfo->ManaCost) + " mana"; break;
                case POWER_RAGE: costText = std::to_string(spellInfo->ManaCost) + " rage"; break;
                case POWER_FOCUS: costText = std::to_string(spellInfo->ManaCost) + " focus"; break;
                case POWER_ENERGY: costText = std::to_string(spellInfo->ManaCost) + " energy"; break;
                case POWER_RUNIC_POWER: costText = std::to_string(spellInfo->ManaCost) + " runic power"; break;
                default: costText = std::to_string(spellInfo->ManaCost) + " unknown resource"; break;
            }
        }
        else
        {
            costText = "no cost";
        }
        
        std::ostringstream line;
        line << "**" << name << "** (ID: " << spellId << ") - " << effectText << ", Costs " << costText << ".";

        // sSpellMgr ranks are 1-based; 0 means unranked, which we treat as a
        // single-rank spell that always wins its own name.
        uint32 rank = sSpellMgr->GetSpellRank(spellId);

        auto existing = bestByName.find(name);
        if (existing == bestByName.end() || rank > existing->second.rank)
            bestByName[name] = { spellId, rank, line.str() };
    }

    for (auto const& entry : bestByName)
        spellSummary << entry.second.line << "\n";

    return spellSummary.str();
}

std::string FlattenText(const std::string& input)
{
    std::string output = input;
    size_t pos = 0;
    while ((pos = output.find('\n', pos)) != std::string::npos)
    {
        output.replace(pos, 1, "|");
        pos += 1;
    }
    return output;
}

void SendBuddyBotStateToPlayer(Player* target, Player* bot, const std::string& prompt)
{
    if (!target || !bot || !g_EnableBotBuddyAddon) return;

    std::string state = prompt;
    std::string::size_type json_pos = state.find("You are an AI-controlled bot");
    if (json_pos != std::string::npos)
        state = state.substr(0, json_pos);

    auto get_section = [&](const std::string& start, const std::string& stop) -> std::string {
        auto s = state.find(start);
        if (s == std::string::npos) return "";
        s += start.size();
        auto e = state.find(stop, s);
        if (e == std::string::npos) e = state.size();
        return state.substr(s, e - s);
    };

    auto get_section_to_end = [&](const std::string& start) -> std::string {
        auto s = state.find(start);
        if (s == std::string::npos) return "";
        s += start.size();
        std::string section = state.substr(s);
        size_t first = section.find_first_not_of(" \r\n\t");
        size_t last = section.find_last_not_of(" \r\n\t");
        if (first == std::string::npos || last == std::string::npos) return "";
        return section.substr(first, last - first + 1);
    };

    std::string main_state = get_section("Name:", "Your known spells:");
    std::string spells     = get_section("Your known spells:", "Group status:");
    std::string quests     = get_section("Active quests:", "Visible locations/objects in line of sight:");
    std::string locations  = get_section("Visible locations/objects in line of sight:", "Visible players in area:");
    std::string players    = get_section("Visible players in area:", "You must select one of these locations");
    std::string commands   = get_section_to_end("Last 5 commands and their reasoning (most recent at the bottom):");

    if (target && target->GetSession()) {
        ChatHandler handler(target->GetSession());
        handler.SendSysMessage(("[BUDDY_STATE] " + FlattenText(main_state + spells + quests)).c_str());
        handler.SendSysMessage(("[BUDDY_LOCATIONS] " + FlattenText(locations)).c_str());
        handler.SendSysMessage(("[BUDDY_PLAYERS] " + FlattenText(players)).c_str());
        handler.SendSysMessage(("[BUDDY_COMMANDS] " + FlattenText(commands)).c_str());
    }
}


std::vector<std::string> GetVisiblePlayers(Player* bot, float radius = 100.0f)
{
    std::vector<std::string> players;
    if (!bot || !bot->GetMap()) return players;

    for (auto const& pair : ObjectAccessor::GetPlayers())
    {
        Player* player = pair.second;
        if (!player || player == bot) continue;
        if (!player->IsInWorld() || player->IsGameMaster()) continue;
        if (player->GetMap() != bot->GetMap()) continue;
        if (!bot->IsWithinDistInMap(player, radius)) continue;
        if (!bot->IsWithinLOS(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ())) continue;

        float dist = bot->GetDistance(player);
        std::string faction = (player->GetTeamId() == TEAM_ALLIANCE ? "Alliance" : "Horde");

        players.push_back(fmt::format(
            "Player: {} (guid: {}, Level: {}, Class: {}, Race: {}, Faction: {}, Position: {:.1f} {:.1f} {:.1f}, Distance: {:.1f})",
            player->GetName(),
            player->GetGUID().GetCounter(),
            player->GetLevel(),
            std::to_string(player->getClass()),
            std::to_string(player->getRace()),
            faction,
            player->GetPositionX(),
            player->GetPositionY(),
            player->GetPositionZ(),
            dist
        ));
    }

    return players;
}

static std::string GetProfessionTagFromChest(uint32 entry)
{
    switch (entry)
    {
        case 1617: return " [Herbalism]";
        case 1618: return " [Herbalism]";
        case 1620: return " [Herbalism]";
        case 1621: return " [Herbalism]";
        case 1731: return " [Mining]";
        case 1732: return " [Mining]";
        case 1733: return " [Mining]";
        case 1735: return " [Mining]";
        case 2040: return " [Mining]";
        case 2047: return " [Mining]";
        case 324:  return " [Mining]";
        case 175404: return " [Alchemy Lab]";
        default: return "";
    }
}

void AddBotCommandHistory(Player* bot, const std::string& command)
{
    if (!bot || command.empty()) return;

    BotControlCommand parsedCommand;

    std::lock_guard<std::mutex> lock(botCommandHistoryMutex);
    uint64_t guid = bot->GetGUID().GetRawValue();
    auto& dq = botCommandHistory[guid];
    dq.push_back(command);
    if (dq.size() > 5) dq.pop_front();
}

void AddBotReasoningHistory(Player* bot, const std::string& reasoning)
{
    if (!bot || reasoning.empty()) return;
    std::lock_guard<std::mutex> lock(botReasoningHistoryMutex);
    uint64_t guid = bot->GetGUID().GetRawValue();
    auto& dq = botReasoningHistory[guid];
    dq.push_back(reasoning);
    if (dq.size() > 5) dq.pop_front();
}


std::vector<std::string> GetBotCommandHistory(Player* bot)
{
    std::vector<std::string> out;
    if (!bot) return out;
    std::lock_guard<std::mutex> lock(botCommandHistoryMutex);
    uint64_t guid = bot->GetGUID().GetRawValue();
    if (botCommandHistory.count(guid))
        out.assign(botCommandHistory[guid].begin(), botCommandHistory[guid].end());
    return out;
}

std::vector<std::string> GetBotReasoningHistory(Player* bot)
{
    std::vector<std::string> out;
    if (!bot) return out;
    std::lock_guard<std::mutex> lock(botReasoningHistoryMutex);
    uint64_t guid = bot->GetGUID().GetRawValue();
    if (botReasoningHistory.count(guid))
        out.assign(botReasoningHistory[guid].begin(), botReasoningHistory[guid].end());
    return out;
}

// Gather visible objects (creatures/gameobjects) around the bot with LOS check
std::vector<std::string> GetVisibleLocations(Player* bot, float radius = 100.0f)
{
    std::vector<std::string> visible;
    if (!bot || !bot->GetMap()) return visible;
    Map* map = bot->GetMap();

    for (auto const& pair : map->GetCreatureBySpawnIdStore())
    {
        Creature* c = pair.second;
        if (!c) continue;
        if (c->GetGUID() == bot->GetGUID()) continue;
        if (!bot->IsWithinDistInMap(c, radius)) continue;
        if (!bot->IsWithinLOS(c->GetPositionX(), c->GetPositionY(), c->GetPositionZ())) continue;
        if (c->IsPet() || c->IsTotem()) continue;

        std::string type;
        if (c->isDead())
        {
            type = "DEAD";
            if (c->hasLootRecipient() && (c->GetLootRecipient() == bot || (c->GetLootRecipientGroup() && bot->GetGroup() == c->GetLootRecipientGroup())))
            {
                type = "DEAD (LOOTABLE)";
            }
            else
            {
                continue;
            }
            if(!c->hasLootRecipient())
            {
                if (c->GetCreatureTemplate() && c->GetCreatureTemplate()->SkinLootId)
                {
                    type += " [SKINNABLE]";
                }
            }
        }
        else if (c->IsHostileTo(bot)) type = "ENEMY";
        else if (c->IsFriendlyTo(bot)) type = "FRIENDLY";
        else type = "NEUTRAL";

        std::string questGiver = "";
        
        // Only consider NPCs that are actually useful to the bot
        if (c->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_QUESTGIVER)) {
            // Check if this quest giver has relevant quests for the bot
            bool hasCompleteQuests = false;
            bool hasAvailableQuests = false;
            
            // Check for completable quests first (highest priority)
            QuestRelationBounds qir = sObjectMgr->GetCreatureQuestInvolvedRelationBounds(c->GetEntry());
            for (QuestRelations::const_iterator itr = qir.first; itr != qir.second; ++itr)
            {
                uint32 questId = itr->second;
                if (bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE && !bot->GetQuestRewardStatus(questId))
                {
                    hasCompleteQuests = true;
                    break;
                }
            }
            
            // Check for available quests (secondary priority)
            if (!hasCompleteQuests)
            {
                QuestRelationBounds qr = sObjectMgr->GetCreatureQuestRelationBounds(c->GetEntry());
                for (QuestRelations::const_iterator itr = qr.first; itr != qr.second; ++itr)
                {
                    uint32 questId = itr->second;
                    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                    if (quest && bot->GetQuestStatus(questId) == QUEST_STATUS_NONE && 
                        bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false))
                    {
                        hasAvailableQuests = true;
                        break;
                    }
                }
            }
            
            // Only show quest giver tags if there are actually relevant quests
            if (hasCompleteQuests) {
                questGiver = " [QUEST GIVER - TURN IN READY]";
            } else if (hasAvailableQuests) {
                questGiver = " [QUEST GIVER - QUESTS AVAILABLE]";
            }
        }
        
        // Check for other useful NPC types (friendly/neutral only) 
        // Handle multiple flags - NPCs can be both quest givers AND vendors/trainers
        if (type == "FRIENDLY" || type == "NEUTRAL") {
            std::vector<std::string> npcTypes;
            
            // Check for vendors
            if (c->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_VENDOR)) {
                npcTypes.push_back("[VENDOR]");
            }
            // Check for trainers
            if (c->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_TRAINER)) {
                npcTypes.push_back("[TRAINER]");
            }
            // Check for flight masters
            if (c->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_FLIGHTMASTER)) {
                npcTypes.push_back("[FLIGHT MASTER]");
            }
            // Check for innkeepers
            if (c->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_INNKEEPER)) {
                npcTypes.push_back("[INNKEEPER]");
            }
            // Check for bankers
            if (c->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_BANKER)) {
                npcTypes.push_back("[BANKER]");
            }
            // Check for auctioneers
            if (c->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_AUCTIONEER)) {
                npcTypes.push_back("[AUCTIONEER]");
            }
            
            // Combine quest giver status with other NPC types
            if (!npcTypes.empty()) {
                if (!questGiver.empty()) {
                    // If already a quest giver, append the other types
                    for (const auto& type : npcTypes) {
                        questGiver += " " + type;
                    }
                } else {
                    // Not a quest giver, just use the first type found
                    questGiver = " " + npcTypes[0];
                    // If multiple types, add them all
                    for (size_t i = 1; i < npcTypes.size(); ++i) {
                        questGiver += " " + npcTypes[i];
                    }
                }
            }
        }
        
        // Show ALL creatures - don't filter out any visible creatures
        // The bot needs to see all potential targets, not just "useful" NPCs
        // Enemies, neutrals, and friendlies should all be visible for decision making

        // Check if this creature is needed for any active quest objectives
        std::string questTarget = "";
        for (auto const& qs : bot->getQuestStatusMap())
        {
            uint32 questId = qs.first;
            QuestStatus status = qs.second.Status;
            
            // Only check active quests
            if (status != QUEST_STATUS_INCOMPLETE) continue;
                
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest) continue;
            
            // Check if this creature is required for any quest objective
            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i) {
                if (quest->RequiredNpcOrGo[i] > 0 && quest->RequiredNpcOrGo[i] == (int32)c->GetEntry()) {
                    uint32 currentCount = bot->GetReqKillOrCastCurrentCount(questId, quest->RequiredNpcOrGo[i]);
                    uint32 requiredCount = quest->RequiredNpcOrGoCount[i];
                    
                    if (currentCount < requiredCount) {
                        questTarget = " [QUEST TARGET - " + quest->GetTitle() + "]";
                        break;
                    }
                }
            }
            if (!questTarget.empty()) break;
        }

        float dist = bot->GetDistance(c);
        visible.push_back(fmt::format(
            "{}: {}{}{} (guid: {}, Level: {}, HP: {}/{}, Position: {} {} {}, Distance: {:.1f})",
            type,
            c->GetName(),
            questGiver,
            questTarget,
            c->GetGUID().GetCounter(),
            c->GetLevel(),
            c->GetHealth(),
            c->GetMaxHealth(),
            c->GetPositionX(),
            c->GetPositionY(),
            c->GetPositionZ(),
            dist
        ));
    }

    for (auto const& pair : map->GetGameObjectBySpawnIdStore())
    {
        GameObject* go = pair.second;
        if (!go) continue;
        if (!bot->IsWithinDistInMap(go, radius)) continue;
        if (!bot->IsWithinLOS(go->GetPositionX(), go->GetPositionY(), go->GetPositionZ())) continue;

        // Only list game objects a bot can meaningfully act on.
        //
        // Every chair, bench and campfire in line of sight was being listed with
        // full coordinates. In a populated inn that is a dozen useless entries
        // crowding out actual targets, and it inflates a ~2000 token prompt that
        // is now paid for once per bot per decision across a whole party.
        switch (go->GetGoType())
        {
            case GAMEOBJECT_TYPE_DOOR:
            case GAMEOBJECT_TYPE_BUTTON:
            case GAMEOBJECT_TYPE_QUESTGIVER:
            case GAMEOBJECT_TYPE_CHEST:
            case GAMEOBJECT_TYPE_GOOBER:
            case GAMEOBJECT_TYPE_SPELLCASTER:
            case GAMEOBJECT_TYPE_FISHINGHOLE:
            case GAMEOBJECT_TYPE_FLAGSTAND:
            case GAMEOBJECT_TYPE_FLAGDROP:
                break;
            default:
                continue;   // chairs, campfires, decor
        }

        std::string tag = "";

        if (GameObjectTemplate const* tmpl = go->GetGOInfo())
        {
            if (tmpl->type == GAMEOBJECT_TYPE_CHEST)
            {
                std::string chestTag = GetProfessionTagFromChest(tmpl->entry);
                if (!chestTag.empty())
                    tag = chestTag;
            }
        }
        
        float dist = bot->GetDistance(go);
        visible.push_back(fmt::format(
            "{}{} (guid: {}, Type: {}, Position: {} {} {}, Distance: {:.1f})",
            go->GetName(),
            tag,
            go->GetGUID().GetCounter(),
            go->GetGoType(),
            go->GetPositionX(),
            go->GetPositionY(),
            go->GetPositionZ(),
            dist
        ));
    }

    // Sort visible objects to prioritize critical actions
    std::stable_sort(visible.begin(), visible.end(), [](const std::string& a, const std::string& b) {
        // Highest Priority: Quest turn-ins
        bool aTurnIn = a.find("TURN IN READY") != std::string::npos;
        bool bTurnIn = b.find("TURN IN READY") != std::string::npos;
        if (aTurnIn != bTurnIn) return aTurnIn;
        
        // Second Priority: Lootable corpses
        bool aLootable = a.find("DEAD (LOOTABLE)") != std::string::npos;
        bool bLootable = b.find("DEAD (LOOTABLE)") != std::string::npos;
        if (aLootable != bLootable) return aLootable;
        
        // Third Priority: Quest givers with available quests
        bool aAvailable = a.find("QUESTS AVAILABLE") != std::string::npos;
        bool bAvailable = b.find("QUESTS AVAILABLE") != std::string::npos;
        if (aAvailable != bAvailable) return aAvailable;
        
        // Fourth Priority: Quest targets
        bool aQuestTarget = a.find("QUEST TARGET") != std::string::npos;
        bool bQuestTarget = b.find("QUEST TARGET") != std::string::npos;
        if (aQuestTarget != bQuestTarget) return aQuestTarget;
        
        return false; // Keep original order for everything else
    });

    return visible;
}

std::string GetCombatSummary(Player* bot)
{
    std::ostringstream oss;
    bool inCombat = bot->IsInCombat();
    Unit* victim = bot->GetVictim();
    
    // Get bot's combat characteristics
    PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
    bool isMelee = ai ? ai->IsMelee(bot) : false;
    bool isRanged = ai ? ai->IsRanged(bot) : false;
    std::string combatType = isMelee ? "MELEE" : (isRanged ? "RANGED" : "HYBRID");

    // Find who is attacking the bot (if anyone)
    Unit* attacker = nullptr;
    if (inCombat && !victim)
    {
        Map* map = bot->GetMap();
        if (map)
        {
            for (auto const& pair : map->GetCreatureBySpawnIdStore())
            {
                Creature* c = pair.second;
                if (!c) continue;
                if (c->GetVictim() == bot)
                {
                    attacker = c;
                    break;
                }
            }
        }
    }

    auto safe_name = [](Unit* unit) -> std::string { return unit ? unit->GetName() : "?"; };
    auto safe_guid = [](Unit* unit) -> std::string { return unit ? std::to_string(unit->GetGUID().GetCounter()) : "?"; };
    auto safe_level = [](Unit* unit) -> std::string { return unit ? std::to_string(unit->GetLevel()) : "?"; };
    auto safe_hp = [](Unit* unit) -> std::string { return unit ? std::to_string(unit->GetHealth()) : "?"; };
    auto safe_maxhp = [](Unit* unit) -> std::string { return unit ? std::to_string(unit->GetMaxHealth()) : "?"; };

    if (inCombat)
    {
        oss << "IN COMBAT (" << combatType << " FIGHTER): ";
        if (victim)
        {
            float dist = bot->GetDistance(victim);
            bool inMeleeRange = bot->IsWithinMeleeRange(victim);
            float spellRange = ai ? ai->GetRange("spell") : 25.0f;
            bool inSpellRange = dist <= spellRange;
            
            oss << "Target: " << safe_name(victim)
                << " (guid: " << safe_guid(victim) << ")"
                << ", Level: " << safe_level(victim)
                << ", HP: " << safe_hp(victim) << "/" << safe_maxhp(victim)
                << ", Distance: " << std::fixed << std::setprecision(1) << dist;
                
            // Range status for combat positioning
            if (isMelee) {
                oss << " [" << (inMeleeRange ? "IN MELEE RANGE" : "TOO FAR FOR MELEE") << "]";
            } else if (isRanged) {
                if (dist < 5.0f) {
                    oss << " [TOO CLOSE - NEED TO BACK AWAY]";
                } else if (inSpellRange) {
                    oss << " [GOOD RANGED POSITION]";
                } else {
                    oss << " [TOO FAR FOR SPELLS]";
                }
            }
        }
        else
        {
            oss << "No current target";
        }
        oss << ". ";

        if (attacker)
        {
            float dist = bot && attacker ? bot->GetDistance(attacker) : -1.0f;

            Creature* c = dynamic_cast<Creature*>(attacker);
            Player* p = dynamic_cast<Player*>(attacker);

            oss << "DEFEND YOURSELF, YOU ARE UNDER ATTACK BY: ";
            if (c)
            {
                // Creature-specific info
                oss << "Creature '" << safe_name(c)
                    << "' (guid: " << safe_guid(c) << ")"
                    << ", Level: " << safe_level(c)
                    << ", HP: " << safe_hp(c) << "/" << safe_maxhp(c)
                    << ", Distance: " << (dist >= 0 ? (std::ostringstream() << std::fixed << std::setprecision(1) << dist).str() : "?")
                    << ", Elite: " << (c->isElite() ? "Yes" : "No");

                // Show auras/buffs/debuffs
                oss << ", Auras:";
                bool anyAura = false;
                for (auto& auraPair : c->GetOwnedAuras())
                {
                    if (!anyAura) anyAura = true;
                    oss << " " << auraPair.second->GetSpellInfo()->SpellName[0];
                }
                if (!anyAura) oss << " None";
            }
            else if (p)
            {
                // Player-specific info
                std::string pFaction = (p->GetTeamId() == TEAM_ALLIANCE ? "Alliance" : "Horde");
                oss << "Player '" << safe_name(p)
                    << "' (guid: " << safe_guid(p) << ")"
                    << ", Level: " << safe_level(p)
                    << ", HP: " << safe_hp(p) << "/" << safe_maxhp(p)
                    << ", Distance: " << (dist >= 0 ? (std::ostringstream() << std::fixed << std::setprecision(1) << dist).str() : "?")
                    << ", Faction: " << pFaction
                    << ", Class: " << std::to_string(p->getClass())
                    << ", Race: " << std::to_string(p->getRace());

                // Show auras/buffs/debuffs
                oss << ", Auras:";
                bool anyAura = false;
                for (auto& auraPair : p->GetOwnedAuras())
                {
                    if (!anyAura) anyAura = true;
                    oss << " " << auraPair.second->GetSpellInfo()->SpellName[0];
                }
                if (!anyAura) oss << " None";
            }
            else
            {
                // Unknown Unit type
                oss << safe_name(attacker)
                    << " (guid: " << safe_guid(attacker) << ")"
                    << ", Level: " << safe_level(attacker)
                    << ", HP: " << safe_hp(attacker) << "/" << safe_maxhp(attacker)
                    << ", Distance: " << (dist >= 0 ? (std::ostringstream() << std::fixed << std::setprecision(1) << dist).str() : "?");
            }

            oss << ". ";
        }

        oss << "Your HP: " << (bot ? std::to_string(bot->GetHealth()) : "?") << "/" << (bot ? std::to_string(bot->GetMaxHealth()) : "?");
        oss << ", Mana: " << (bot ? std::to_string(bot->GetPower(POWER_MANA)) : "?") << "/" << (bot ? std::to_string(bot->GetMaxPower(POWER_MANA)) : "?");
        oss << ", Energy: " << (bot ? std::to_string(bot->GetPower(POWER_ENERGY)) : "?") << "/" << (bot ? std::to_string(bot->GetMaxPower(POWER_ENERGY)) : "?");
    }
    else
    {
        oss << "NOT IN COMBAT (" << combatType << " FIGHTER). ";
        
        // Check for health issues that might indicate environmental damage
        if (bot) {
            float healthPercent = (float)bot->GetHealth() / (float)bot->GetMaxHealth() * 100.0f;
            if (healthPercent < 90.0f) {
                oss << "WARNING: Your health is at " << (int)healthPercent << "% - you may be taking environmental damage! ";
            }
        }
        
        oss << "Your HP: " << (bot ? std::to_string(bot->GetHealth()) : "?") << "/" << (bot ? std::to_string(bot->GetMaxHealth()) : "?");
        oss << ", Mana: " << (bot ? std::to_string(bot->GetPower(POWER_MANA)) : "?") << "/" << (bot ? std::to_string(bot->GetMaxPower(POWER_MANA)) : "?");
        oss << ", Energy: " << (bot ? std::to_string(bot->GetPower(POWER_ENERGY)) : "?") << "/" << (bot ? std::to_string(bot->GetMaxPower(POWER_ENERGY)) : "?");
    }
    return oss.str();
}


std::string GetDetailedQuestInfo(Player* bot)
{
    std::ostringstream oss;
    
    bool hasActiveQuests = false;
    
    for (auto const& qs : bot->getQuestStatusMap())
    {
        uint32 questId = qs.first;
        QuestStatus status = qs.second.Status;
        
        // Skip abandoned, failed, or already rewarded quests
        if (status == QUEST_STATUS_NONE || status == QUEST_STATUS_FAILED || status == QUEST_STATUS_REWARDED)
            continue;
            
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest) continue;
        
        if (!hasActiveQuests) {
            oss << "Active quests:\n";
            hasActiveQuests = true;
        }
        
        std::string statusText;
        switch (status) {
            case QUEST_STATUS_INCOMPLETE: statusText = "IN PROGRESS"; break;
            case QUEST_STATUS_COMPLETE: statusText = "READY TO TURN IN"; break;
            default: statusText = "UNKNOWN"; break;
        }
        
        oss << "\n**QUEST: " << quest->GetTitle() << "** (ID: " << questId << ") - " << statusText << "\n";
        oss << "Level: " << quest->GetQuestLevel() << " | XP Reward: " << quest->XPValue(bot->GetLevel()) << "\n";
        
        if (status == QUEST_STATUS_COMPLETE) {
            oss << "*** PRIORITY: FIND QUEST GIVER TO TURN IN THIS QUEST ***\n";
            
            // Find who can accept this quest turn-in
            std::vector<std::string> turnInNPCs;
            
            // Check creatures that can accept this quest
            QuestRelationBounds qir = sObjectMgr->GetCreatureQuestInvolvedRelationBounds(questId);
            for (QuestRelations::const_iterator itr = qir.first; itr != qir.second; ++itr) {
                CreatureTemplate const* cTemplate = sObjectMgr->GetCreatureTemplate(itr->first);
                if (cTemplate) {
                    turnInNPCs.push_back(std::string("NPC: ") + cTemplate->Name);
                }
            }
            
            // Check game objects that can accept this quest
            QuestRelationBounds goQir = sObjectMgr->GetGOQuestInvolvedRelationBounds(questId);
            for (QuestRelations::const_iterator itr = goQir.first; itr != goQir.second; ++itr) {
                GameObjectTemplate const* goTemplate = sObjectMgr->GetGameObjectTemplate(itr->first);
                if (goTemplate) {
                    turnInNPCs.push_back(std::string("Object: ") + goTemplate->name);
                }
            }
            
            if (!turnInNPCs.empty()) {
                oss << "Turn in to: ";
                for (size_t i = 0; i < turnInNPCs.size(); ++i) {
                    oss << turnInNPCs[i];
                    if (i < turnInNPCs.size() - 1) oss << " OR ";
                }
                oss << "\n";
            }
        } else {
            // Quest is incomplete - show objectives
            oss << "Objectives to complete:\n";
            
            // Check kill objectives
            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i) {
                if (quest->RequiredNpcOrGo[i] != 0) {
                    uint32 currentCount = bot->GetReqKillOrCastCurrentCount(questId, quest->RequiredNpcOrGo[i]);
                    uint32 requiredCount = quest->RequiredNpcOrGoCount[i];
                    
                    if (requiredCount > 0) {
                        std::string targetName = "Unknown Target";
                        
                        if (quest->RequiredNpcOrGo[i] > 0) {
                            // It's a creature
                            CreatureTemplate const* cTemplate = sObjectMgr->GetCreatureTemplate(quest->RequiredNpcOrGo[i]);
                            if (cTemplate) {
                                targetName = std::string("Kill ") + cTemplate->Name;
                            }
                        } else {
                            // It's a game object (negative value)
                            GameObjectTemplate const* goTemplate = sObjectMgr->GetGameObjectTemplate(-quest->RequiredNpcOrGo[i]);
                            if (goTemplate) {
                                targetName = std::string("Use/Click ") + goTemplate->name;
                            }
                        }
                        
                        oss << " - " << targetName << ": " << currentCount << "/" << requiredCount;
                        if (currentCount >= requiredCount) {
                            oss << " COMPLETE";
                        } else {
                            oss << " NEED " << (requiredCount - currentCount) << " MORE";
                        }
                        oss << "\n";
                    }
                }
            }
            
            // Check item objectives
            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i) {
                if (quest->RequiredItemId[i] != 0) {
                    uint32 currentCount = bot->GetItemCount(quest->RequiredItemId[i], true);
                    uint32 requiredCount = quest->RequiredItemCount[i];
                    
                    if (requiredCount > 0) {
                        ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(quest->RequiredItemId[i]);
                        std::string itemName = itemTemplate ? itemTemplate->Name1 : "Unknown Item";
                        
                        oss << " - Collect " << itemName << ": " << currentCount << "/" << requiredCount;
                        if (currentCount >= requiredCount) {
                            oss << " COMPLETE";
                        } else {
                            oss << " NEED " << (requiredCount - currentCount) << " MORE";
                        }
                        oss << "\n";
                    }
                }
            }
            
            // Check exploration objectives
            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i) {
                if (quest->RequiredNpcOrGo[i] == 0 && quest->RequiredNpcOrGoCount[i] > 0) {
                    // This might be an exploration or spell cast objective
                    uint32 currentCount = bot->GetReqKillOrCastCurrentCount(questId, quest->RequiredNpcOrGo[i]);
                    uint32 requiredCount = quest->RequiredNpcOrGoCount[i];
                    
                    if (requiredCount > 0) {
                        oss << " - Exploration/Event objective: " << currentCount << "/" << requiredCount;
                        if (currentCount >= requiredCount) {
                            oss << " COMPLETE";
                        } else {
                            oss << " INCOMPLETE";
                        }
                        oss << "\n";
                    }
                }
            }
            
            // Show quest description for context
            if (!quest->GetObjectives().empty()) {
                oss << "Description: " << quest->GetObjectives() << "\n";
            }
        }
    }
    
    if (!hasActiveQuests) {
        oss << "No active quests. Look for quest givers with available quests or turn-ins ready!\n";
    }
    
    return oss.str();
}

std::vector<std::string> GetNearbyWaypoints(Player* bot, float radius = 200.0f)
{
    std::vector<std::string> wps;
    if (!bot) return wps;
    uint32 bot_map = bot->GetMapId();
    float bot_x = bot->GetPositionX();
    float bot_y = bot->GetPositionY();
    float bot_z = bot->GetPositionZ();

    auto nodes = sTravelNodeMap.getNodes();
    int idx = 0;
    for (TravelNode* node : nodes)
    {
        if (!node) continue;
        WorldPosition* pos = node->getPosition();
        if (!pos) continue;
        if (pos->GetMapId() != bot_map) continue;
        float dx = pos->GetPositionX() - bot_x;
        float dy = pos->GetPositionY() - bot_y;
        float dz = pos->GetPositionZ() - bot_z;
        float dist = sqrtf(dx*dx + dy*dy + dz*dz);
        if (dist > radius) continue;
        wps.push_back(fmt::format("Node #{} '{}' ({:.1f}, {:.1f}, {:.1f}), distance: {:.1f}", idx, node->getName(), pos->GetPositionX(), pos->GetPositionY(), pos->GetPositionZ(), dist));        
        ++idx;
    }
    return wps;
}


// ---------------------------------------------------------------------------
// Structured output schema (Ollama "format" field).
//
// Passing format:"json" only guarantees syntactic validity — the model still
// invented command types, target guids and coordinates on other continents.
// Passing a full JSON Schema makes Ollama constrain generation at the decoder,
// so invalid output becomes structurally impossible rather than merely
// discouraged. Two constraints do the heavy lifting:
//
//   * guid  -> enum of guids actually visible to this bot right now.
//   * x/y/z -> numeric bounds around the bot's current position, which stops
//              the model emitting memorised coordinates from another zone.
//
// Must be called on the world thread (it touches the bot's map).
// ---------------------------------------------------------------------------
static nlohmann::json BuildBotActionSchema(Player* bot, size_t destCount, float radius = 100.0f)
{
    std::vector<uint32_t> guids;          // anything visible: interact, spell targets
    std::vector<uint32_t> attackableGuids; // only things the bot may actually attack
    std::vector<uint32_t> spellIds;

    if (bot && bot->GetMap())
    {
        Map* map = bot->GetMap();

        for (auto const& pair : map->GetCreatureBySpawnIdStore())
        {
            Creature* c = pair.second;
            if (!c) continue;
            if (c->GetGUID() == bot->GetGUID()) continue;
            if (!bot->IsWithinDistInMap(c, radius)) continue;
            if (!bot->IsWithinLOS(c->GetPositionX(), c->GetPositionY(), c->GetPositionZ())) continue;
            if (c->IsPet() || c->IsTotem()) continue;
            guids.push_back(c->GetGUID().GetCounter());

            // Attack targets are a strict subset. The shared guid enum contains
            // every visible creature, so "attack the friendly quest giver" was a
            // legal move in the grammar, and the model took it: 9 of 87 attack
            // commands were rejected downstream as unattackable, each one a
            // wasted decision cycle.
            if (!c->isDead() && bot->IsValidAttackTarget(c))
                attackableGuids.push_back(c->GetGUID().GetCounter());
        }

        for (auto const& pair : map->GetGameObjectBySpawnIdStore())
        {
            GameObject* go = pair.second;
            if (!go) continue;
            if (!bot->IsWithinDistInMap(go, radius)) continue;
            if (!bot->IsWithinLOS(go->GetPositionX(), go->GetPositionY(), go->GetPositionZ())) continue;
            guids.push_back(go->GetGUID().GetCounter());
        }

        for (auto const& pair : bot->GetSpellMap())
        {
            if (!pair.second || pair.second->State == PLAYERSPELL_REMOVED || !pair.second->Active)
                continue;
            spellIds.push_back(pair.first);
        }
    }

    // guid: restrict to what the bot can actually see. If nothing is visible we
    // fall back to a plain integer rather than an empty enum, which no value
    // could satisfy.
    nlohmann::json guidSchema = guids.empty()
        ? nlohmann::json{{"type", "integer"}}
        : nlohmann::json{{"type", "integer"}, {"enum", guids}};

    // If nothing is attackable, the field is omitted entirely rather than left
    // unconstrained: an empty enum satisfies nothing, and a free integer is an
    // escape hatch the model will happily use.
    nlohmann::json attackGuidSchema = attackableGuids.empty()
        ? nlohmann::json()
        : nlohmann::json{{"type", "integer"}, {"enum", attackableGuids}};

    nlohmann::json spellSchema = spellIds.empty()
        ? nlohmann::json{{"type", "integer"}}
        : nlohmann::json{{"type", "integer"}, {"enum", spellIds}};

    // Coordinate bounds. Generous enough for real exploration, tight enough to
    // exclude another continent.
    const float kXYRange = 500.0f;
    const float kZRange  = 250.0f;
    float bx = bot ? bot->GetPositionX() : 0.0f;
    float by = bot ? bot->GetPositionY() : 0.0f;
    float bz = bot ? bot->GetPositionZ() : 0.0f;

    std::vector<uint32_t> destIndices;
    for (uint32_t i = 0; i < static_cast<uint32_t>(destCount); ++i) destIndices.push_back(i);
    nlohmann::json destIndexSchema = destIndices.empty()
        ? nlohmann::json{{"type", "integer"}}
        : nlohmann::json{{"type", "integer"}, {"enum", destIndices}};

    auto bounded = [](float centre, float range) {
        return nlohmann::json{
            {"type", "number"},
            {"minimum", centre - range},
            {"maximum", centre + range}
        };
    };

    // When we have a validated destination list, REMOVE x/y/z from the grammar
    // entirely. Offering both let the model keep emitting raw coordinates (and
    // it did: it invented -9000,-1000,500 while standing at -707,2734). A
    // parameter that does not exist in the grammar cannot be produced.
    nlohmann::json paramProps = {
        {"guid", guidSchema},
        {"spellid", spellSchema},
        {"id", {{"type", "integer"}}}
    };
    if (!attackGuidSchema.is_null())
        paramProps["attack_guid"] = attackGuidSchema;

    if (destIndices.empty())
    {
        paramProps["x"] = bounded(bx, kXYRange);
        paramProps["y"] = bounded(by, kXYRange);
        paramProps["z"] = bounded(bz, kZRange);
    }
    else
    {
        paramProps["destination_index"] = destIndexSchema;
    }

    // One schema per command type, combined with oneOf.
    //
    // A single flat params object offered every field to every command, so
    // "attack" could carry a guid naming a friendly quest giver. Adding a
    // separate attack_guid only halved it: measured 9 invalid targets in 183
    // attacks, with just 52% of attacks using the constrained field at all.
    //
    // Per-command schemas make the wrong field unrepresentable rather than
    // merely discouraged. Verified against the model: 8/8 replies carried
    // exactly the right parameter for every command, with no cross-use.
    auto makeCommand = [](char const* name, nlohmann::json props, nlohmann::json required)
    {
        return nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"type", {{"type", "string"}, {"enum", nlohmann::json::array({name})}}},
                {"params", {{"type", "object"}, {"properties", props}, {"required", required}}}
            }},
            {"required", {"type", "params"}}
        };
    };

    nlohmann::json moveParams = nlohmann::json::object();
    nlohmann::json moveRequired = nlohmann::json::array();
    if (!destIndices.empty())
    {
        moveParams["destination_index"] = destIndexSchema;
        moveRequired.push_back("destination_index");
    }
    else
    {
        moveParams["x"] = bounded(bx, kXYRange);
        moveParams["y"] = bounded(by, kXYRange);
        moveParams["z"] = bounded(bz, kZRange);
        moveRequired = nlohmann::json::array({"x", "y", "z"});
    }

    nlohmann::json variants = nlohmann::json::array();
    variants.push_back(makeCommand("move_to", moveParams, moveRequired));

    // Only offer attack when something is actually attackable.
    if (!attackGuidSchema.is_null())
        variants.push_back(makeCommand("attack",
            {{"attack_guid", attackGuidSchema}}, nlohmann::json::array({"attack_guid"})));

    variants.push_back(makeCommand("interact",
        {{"guid", guidSchema}}, nlohmann::json::array({"guid"})));

    variants.push_back(makeCommand("spell",
        {{"spellid", spellSchema}, {"guid", guidSchema}}, nlohmann::json::array({"spellid"})));

    for (char const* simple : {"loot", "follow", "stop"})
        variants.push_back(makeCommand(simple, nlohmann::json::object(), nlohmann::json::array()));

    for (char const* questCmd : {"accept_quest", "turn_in_quest"})
        variants.push_back(makeCommand(questCmd,
            {{"quest_id", {{"type", "integer"}}}}, nlohmann::json::array({"quest_id"})));

    nlohmann::json commandSchema = {{"oneOf", variants}};

    return nlohmann::json{
        {"type", "object"},
        {"properties", {
            {"command", commandSchema},
            // A short ordered plan. The grammar caps it so the model cannot
            // emit a sprawling sequence that is stale by the time it runs.
            {"intent", {{"type", "string"}}},
            {"steps", {
                {"type", "array"},
                {"minItems", 1},
                {"maxItems", 4},
                {"items", commandSchema}
            }},
            {"reasoning", {{"type", "string"}}},
            {"say", {{"type", "string"}}}
        }},
        {"required", {"steps"}}
    };
}

OllamaBotControlLoop::OllamaBotControlLoop() : WorldScript("OllamaBotControlLoop") {}

static std::unordered_map<uint64_t, time_t> nextTick;

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
    std::string* responseBuffer = static_cast<std::string*>(userp);
    size_t totalSize = size * nmemb;
    responseBuffer->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

static std::string QueryOllamaLLM(const std::string& prompt, const nlohmann::json& schema)
{
    CURL* curl = curl_easy_init();
    if (!curl)
    {
        LOG_INFO("server.loading", "[OllamaBotBuddy] Failed to initialize cURL.");
        return "";
    }

    nlohmann::json requestData = {
        {"model",  g_OllamaBotControlModel},
        {"prompt", prompt},
        // Structured outputs: a full JSON Schema (not just "json") makes Ollama
        // constrain decoding to the exact command shape, with guids limited to
        // visible targets and coordinates bounded to the bot's vicinity.
        {"format", schema},
        // Cap the reply: a single command is short. Uncapped, a 1B model will
        // ramble for >1700 tokens, which dominates request latency.
        {"options", {
            {"num_predict", 200},
            {"temperature", 0.2}
        }}
    };
    std::string requestDataStr = requestData.dump();

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    std::string responseBuffer;
    curl_easy_setopt(curl, CURLOPT_URL, g_OllamaBotControlUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestDataStr.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, long(requestDataStr.length()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBuffer);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
    {
        LOG_INFO("server.loading", "[OllamaBotBuddy] Failed to reach Ollama AI. cURL error: {}", curl_easy_strerror(res));
        return "";
    }

    std::stringstream ss(responseBuffer);
    std::string line, extracted;
    while (std::getline(ss, line))
    {
        try
        {
            nlohmann::json jsonResponse = nlohmann::json::parse(line);
            if (jsonResponse.contains("response"))
                extracted += jsonResponse["response"].get<std::string>();
        }
        catch (...) {}
    }
    return extracted;
}


// ---------------------------------------------------------------------------
// Destination candidates.
//
// Ollama's structured outputs compile the JSON Schema down to a GBNF grammar.
// Grammars can express `enum` (it is just alternation) but cannot express
// numeric `minimum`/`maximum` — those are accepted and then silently ignored.
// Verified directly: a schema demanding -1207..-207 still returned -9000.
//
// So free-form x/y/z can never be constrained by the schema, and models happily
// emit coordinates memorised from other zones. Every such command is then
// rejected by the >500y guard in ParseAndExecuteBotJson, so the bot burns a
// full decision cycle doing nothing.
//
// Instead we precompute a short list of real, reachable destinations and let
// the model choose one by index. An index enum IS enforceable, so an
// unreachable destination becomes impossible to express rather than merely
// invalid.
// ---------------------------------------------------------------------------
// Computed on the world thread; read later by the reply-handling thread.
static std::vector<BotDestination> BuildDestinationCandidates(Player* bot, float radius = 100.0f)
{
    std::vector<BotDestination> out;
    if (!bot || !bot->GetMap()) return out;

    Map* map = bot->GetMap();
    float bx = bot->GetPositionX();
    float by = bot->GetPositionY();
    float bz = bot->GetPositionZ();

    auto pathable = [&](float x, float y, float z) {
        PathGenerator p(bot);
        p.CalculatePath(x, y, z, false);
        return !(p.GetPathType() & PATHFIND_NOPATH);
    };

    // 1. Stand next to something the bot can actually see, NEAREST FIRST.
    //
    // Both the ordering and the distance in the label matter. The spawn-id store
    // is a hash map, so iterating it yields creatures in arbitrary order. Two
    // things went wrong because of that:
    //
    //   * The old 12-entry cap truncated in hash order, so in a crowded zone the
    //     creature standing next to the bot could be missing from the menu while
    //     a creature 95 yards away was offered.
    //   * Every duplicate spawn produced the identical label ("next to Bonechewer
    //     Ravener"), leaving the model no way to tell them apart.
    //
    // Measured with a kill-quest and three identical Raveners at 6y, 82y and 91y:
    // unsorted and unlabelled the model picked the nearest 0/12 times (it always
    // took the first entry, walking 82 yards past one 6 yards away). Sorted, with
    // the distance in the label, it picked the nearest 12/12.
    //
    // Collecting by cheap distance first and only then raycasting also means the
    // expensive IsWithinLOS calls land on the closest creatures, which are the
    // ones most likely to pass -- so this does strictly less work than before.
    std::vector<std::pair<float, Creature*>> nearby;
    for (auto const& pair : map->GetCreatureBySpawnIdStore())
    {
        Creature* c = pair.second;
        if (!c) continue;
        if (c->GetGUID() == bot->GetGUID()) continue;
        if (c->IsPet() || c->IsTotem()) continue;
        if (!bot->IsWithinDistInMap(c, radius)) continue;
        nearby.emplace_back(bot->GetExactDist(c), c);
    }

    std::sort(nearby.begin(), nearby.end(),
              [](auto const& a, auto const& b) { return a.first < b.first; });

    for (auto const& entry : nearby)
    {
        if (out.size() >= 12) break;
        Creature* c = entry.second;
        if (!bot->IsWithinLOS(c->GetPositionX(), c->GetPositionY(), c->GetPositionZ())) continue;

        std::ostringstream label;
        label << "next to " << c->GetName() << " (" << uint32(entry.first + 0.5f) << "y)";
        out.push_back({ label.str(),
                        c->GetPositionX(), c->GetPositionY(), c->GetPositionZ() });
    }

    // 1b. A step toward the bot's quest objective.
    //
    // Measured: 45.5% of the decisions that reach the model are bots holding a
    // quest whose objective is nowhere near them, and the entire movement
    // vocabulary reaches about 100 yards -- on live prompts the furthest option
    // offered had a median of 76 yards and a maximum of 103. A bot whose
    // objective is in the next zone therefore cannot express going there, no
    // matter whether the decision is made here or by the model. That is a
    // navigation gap, not a reasoning one.
    //
    // quest_poi already carries the answer, and every one of the 387 distinct
    // incomplete quests these bots hold has POI data. sObjectMgr keeps it in
    // memory, so this costs a hash lookup rather than a query.
    //
    // The destination offered is deliberately a *step*, not the objective
    // itself: a single MovePoint across a zone will not path, so this projects a
    // short distance along the bearing and checks that much is walkable. Repeated
    // decisions then walk the bot there.
    {
        float bestDist = 0.0f;          // 0 means "nothing found yet"
        float bx2 = 0.0f, by2 = 0.0f;
        std::string bestQuest;

        for (auto const& qs : bot->getQuestStatusMap())
        {
            if (qs.second.Status != QUEST_STATUS_INCOMPLETE) continue;
            Quest const* q = sObjectMgr->GetQuestTemplate(qs.first);
            if (!q) continue;

            QuestPOIVector const* pois = sObjectMgr->GetQuestPOIVector(qs.first);
            if (!pois) continue;

            for (QuestPOI const& poi : *pois)
            {
                if (poi.MapId != map->GetId()) continue;     // same map only
                for (QuestPOIPoint const& pt : poi.points)
                {
                    float const px = float(pt.x), py = float(pt.y);
                    float const d = std::sqrt((px - bx) * (px - bx) + (py - by) * (py - by));

                    // Only worth offering when it is beyond what the local
                    // candidates already cover, and not absurdly far. Nearest
                    // wins: a bot should walk to its closest objective, not its
                    // most distant one.
                    if (d > 120.0f && d < 4000.0f && (bestDist == 0.0f || d < bestDist))
                    {
                        bestDist = d;
                        bx2 = px;
                        by2 = py;
                        bestQuest = q->GetTitle();
                    }
                }
            }
        }

        if (bestDist > 0.0f)
        {
            float const ang = std::atan2(by2 - by, bx2 - bx);
            // Try progressively shorter steps: terrain often blocks the first.
            for (float step : { 80.0f, 50.0f, 25.0f })
            {
                float nx = bx + std::cos(ang) * step;
                float ny = by + std::sin(ang) * step;
                float nz = map->GetHeight(nx, ny, bz + 5.0f, true);
                if (nz <= INVALID_HEIGHT) continue;
                if (!pathable(nx, ny, nz)) continue;

                std::ostringstream lbl;
                lbl << "toward your quest objective: " << bestQuest
                    << " (" << uint32(bestDist) << "y away)";
                out.push_back({ lbl.str(), nx, ny, nz });
                break;
            }
        }
    }

    // 2. Cardinal exploration points, only if genuinely pathable.
    static const struct { char const* name; float dx, dy; } kDirs[] = {
        { "north", 0.0f, 60.0f }, { "south", 0.0f, -60.0f },
        { "east",  60.0f, 0.0f }, { "west", -60.0f, 0.0f },
    };
    for (auto const& d : kDirs)
    {
        float nx = bx + d.dx, ny = by + d.dy;
        float nz = map->GetHeight(nx, ny, bz + 5.0f, true);
        if (nz <= INVALID_HEIGHT) continue;
        if (!pathable(nx, ny, nz)) continue;
        out.push_back({ std::string("explore ") + d.name + " (60y)", nx, ny, nz });
    }

    return out;
}

// ---------------------------------------------------------------------------
// Personality-driven decisions.
//
// mod-ollama-chat assigns each bot a personality and stores it in
// mod_ollama_chat_personality. Its templates only shape speech ("Talk about
// rare loot"), so on their own a bot sounds like a loot goblin while behaving
// identically to everyone else.
//
// We read the same assignment -- one source of truth, no second personality
// system -- and translate it into directives for the action prompt. The chat
// module keeps owning what a bot says; this decides what that disposition
// means for what it does.
// ---------------------------------------------------------------------------
static std::string ActionTraitsFor(std::string const& key)
{
    // Only personalities with a genuine behavioural reading are mapped. A bard
    // rhyming does not imply anything about target selection, and inventing a
    // meaning would just add noise to the prompt.
    static std::unordered_map<std::string, std::string> const kTraits =
    {
        {"LOOTGOBLIN",     "Loot every lootable corpse before anything else. Prefer chests and containers over combat."},
        {"GOBLIN_MERCHANT","Prioritise loot, chests and anything sellable. Avoid fights that offer no profit."},
        {"TRADER",         "Favour gathering and looting over combat; value anything that can be sold."},
        {"GAMER",          "Play efficiently: pick the fastest route to the objective and avoid wasted actions."},
        {"RAIDER",         "Stay with the group, focus the target others are on, and avoid pulling extra enemies."},
        {"PVP_HARDCORE",   "Prefer attacking enemy players over creatures. Engage aggressively when one is visible."},
        {"HEROIC_LEADER",  "Lead from the front. Engage first and defend any group member under attack."},
        {"FANATIC",        "Attack faction enemies on sight, even at unfavourable odds."},
        {"RAGER",          "Attack aggressively and refuse to retreat, even when badly hurt."},
        {"EDGE_LORD",      "Seek out the strongest enemy available and fight it alone."},
        {"WANNABE_VILLAIN","Pick fights you can win for show, and take credit by looting the spoils."},
        {"LONE_WOLF",      "Keep away from other players. Avoid grouping and fight alone."},
        {"PARANOID",       "Assume nearby players will steal your kills and loot. Grab loot first and retreat early."},
        {"CONSPIRACY_THEORIST","Behave erratically: change target or destination often for no clear reason."},
        {"GLITCHED_AI",    "Behave erratically: abandon actions partway and switch to unrelated ones."},
        {"FOOL",           "Frequently pick a poor target or destination. Forget to loot. Wander off mid-task."},
        {"YOUNG_APPRENTICE","Stay near other players and copy what they are doing. Avoid dangerous fights."},
        {"MENTOR",         "Prioritise helping group members over your own progress."},
        {"CASUAL",         "Prefer questing and exploring over grinding. Do not seek out hard fights."},
        {"STONER",         "Act slowly and without urgency. Wander and explore rather than pursuing goals."},
        {"GRUMPY_VETERAN", "Avoid unnecessary risk. Take the efficient, well-trodden option every time."},
        {"SCHOLAR",        "Investigate: prefer interacting with objects and quest givers over combat."},
        {"NPC_IMPERSONATOR","Prefer quest givers and quest objectives over open combat."},
        {"PIRATE",         "Chase loot and plunder. Attack for spoils rather than for objectives."},
        {"HYPE_MAN",       "Stay close to group members and join whatever fight they are already in."},
        {"FLIRT",          "Stay near other players rather than going off alone."},
        {"TRICKSTER",      "Prefer opportunistic targets: wounded enemies and ones others are already fighting."},

        // Realistic player archetypes rather than roleplay personas: the things
        // that actually make a server feel populated by people.
        {"NINJA_LOOTER",   "Loot everything immediately, including corpses others fought for. Never wait your turn."},
        {"ELITIST",        "Only engage fights worth your time. Ignore weak enemies and do not help with trivial tasks."},
        {"AFK_LEECH",      "Act slowly and do the minimum. Follow others rather than choosing your own targets."},
        {"DRAMA_QUEEN",    "Retreat at the first sign of damage and make it obvious. Prefer visible actions over useful ones."},
        {"SWEATY_TRYHARD", "Take the most efficient action every time. Never waste a step or attack a low-value target."},
        {"CHATTERBOX",     "Prefer staying near other players over pursuing your own objectives."},
    };

    auto it = kTraits.find(key);
    return it == kTraits.end() ? std::string() : it->second;
}

// Assigned personality per bot, loaded once per server run.
static std::unordered_map<uint64_t, std::string> g_botPersonalityCache;
static std::unordered_set<uint64_t> g_botPersonalityLoaded;
static std::mutex g_botPersonalityMutex;

static std::string GetBotPersonalityKey(Player* bot)
{
    if (!bot) return "";
    uint64_t raw = bot->GetGUID().GetRawValue();

    {
        std::lock_guard<std::mutex> lock(g_botPersonalityMutex);
        if (g_botPersonalityLoaded.count(raw))
            return g_botPersonalityCache[raw];
    }

    std::string key;
    if (QueryResult result = CharacterDatabase.Query(
            "SELECT personality FROM mod_ollama_chat_personality WHERE guid = {}",
            bot->GetGUID().GetCounter()))
    {
        key = (*result)[0].Get<std::string>();
    }

    // Fall back to a stable, GUID-derived disposition.
    //
    // mod-ollama-chat writes that table lazily, only once a bot actually says
    // something, and bots rarely say anything: measured on the live server there
    // were 18 rows for 1,501 characters, so about 99% of bots reached the prompt
    // with no disposition at all and the whole block was skipped.
    //
    // Deriving it from the GUID instead means every bot has one, it is stable
    // across restarts and needs no write, and the mix is a deliberate choice
    // rather than a side effect of who happened to talk. A bot that does have a
    // chat personality keeps it, so speech and behaviour stay consistent.
    //
    // The weights are the point: a server should read as mostly ordinary people
    // with the occasional memorable one, not a cast of characters. Roughly 70%
    // unremarkable, 20% mildly distinctive, 10% actively disruptive. Roleplay
    // personas are deliberately excluded.
    if (key.empty())
    {
        struct Weighted { char const* key; uint32 weight; };
        static const Weighted kDispositions[] = {
            // Ordinary players getting on with the game (~70%).
            { "CASUAL",          18 },
            { "GAMER",           14 },
            { "GRUMPY_VETERAN",  12 },
            { "RAIDER",          10 },
            { "LONE_WOLF",        8 },
            { "SCHOLAR",          8 },
            // Mildly distinctive, noticeable but not a problem (~20%).
            { "TRICKSTER",        7 },
            { "ELITIST",          6 },
            { "STONER",           5 },
            { "PARANOID",         2 },
            // Genuinely disruptive, deliberately rare (~10%).
            { "EDGE_LORD",        3 },
            { "NINJA_LOOTER",     3 },
            { "RAGER",            2 },
            { "AFK_LEECH",        1 },
            { "FOOL",             1 },
        };

        uint32 total = 0;
        for (auto const& d : kDispositions) total += d.weight;

        // Mix the guid so neighbouring spawn ids do not land on the same bucket.
        uint64_t h = raw * 0x9E3779B97F4A7C15ull;
        h ^= (h >> 31);
        uint32 roll = uint32(h % total);

        for (auto const& d : kDispositions)
        {
            if (roll < d.weight) { key = d.key; break; }
            roll -= d.weight;
        }
    }

    std::lock_guard<std::mutex> lock(g_botPersonalityMutex);
    g_botPersonalityCache[raw] = key;
    g_botPersonalityLoaded.insert(raw);
    return key;
}

// ---------------------------------------------------------------------------
// Situation assessment.
//
// Measured on the eval harness against identical game state, the model failed
// two obvious situations outright:
//
//   * quest giver at distance 0.0, no active quests -> chose move_to 18/18
//   * 16% health while in combat                    -> chose attack  18/18
//
// It was not incapable, it simply never connected state to response. Naming the
// situation fixed the first completely (interact 18/18). Naming the specific
// defensive spell fixed the second (spell 16/18); a general "use a defensive
// ability" only moved it to 5/18. A 7B model does not reliably infer "I have
// Shield Wall and I am at 16%, therefore cast it" -- it has to be told.
//
// Everything here is computed from real state: the bot's own spellbook,
// cooldowns, health, and the distance to things it can actually interact with.
// ---------------------------------------------------------------------------

// Defensive cooldowns and self-heals worth naming, per class. First one the bot
// actually knows and has off cooldown wins.
static uint32 FindUsableDefensive(Player* bot, std::string& nameOut)
{
    if (!bot) return 0;

    static std::unordered_map<uint8, std::vector<uint32>> const kDefensives =
    {
        { CLASS_WARRIOR,      { 871, 12975, 55694 } },          // Shield Wall, Last Stand, Enraged Regen
        { CLASS_PALADIN,      { 642, 498, 633, 1038 } },        // Divine Shield, Divine Protection, LoH
        { CLASS_DEATH_KNIGHT, { 48792, 48707, 55233 } },        // Icebound Fortitude, AMS, Vampiric Blood
        { CLASS_ROGUE,        { 5277, 31224, 26669 } },         // Evasion, Cloak of Shadows, Evasion rank
        { CLASS_PRIEST,       { 19236, 17, 586 } },             // Desperate Prayer, PW:S, Fade
        { CLASS_MAGE,         { 45438, 11958, 66 } },           // Ice Block, Cold Snap, Invisibility
        { CLASS_WARLOCK,      { 47860, 6229, 5697 } },          // Death Coil, Shadow Ward
        { CLASS_HUNTER,       { 5384, 19263, 781 } },           // Feign Death, Deterrence, Disengage
        { CLASS_SHAMAN,       { 30823, 8178, 2825 } },          // Shamanistic Rage, Grounding
        { CLASS_DRUID,        { 22812, 61336, 22842 } },        // Barkskin, Survival Instincts, Frenzied Regen
    };

    auto it = kDefensives.find(bot->getClass());
    if (it == kDefensives.end()) return 0;

    for (uint32 spellId : it->second)
    {
        if (!bot->HasSpell(spellId)) continue;
        if (bot->HasSpellCooldown(spellId)) continue;

        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId))
        {
            char const* n = info->SpellName[0];
            nameOut = (n && *n) ? n : "your defensive ability";
            return spellId;
        }
    }
    return 0;
}

// When outCommand is supplied, each branch that writes a SITUATION line also
// records the command that line is asking for, in the exact JSON shape
// ParseAndExecuteBotJson consumes. The prompt text and the deterministic action
// therefore come from one scan and one set of conditions, and cannot drift apart
// -- which is the invariant several earlier bugs came down to.
static std::string BuildSituationAssessment(Player* bot, nlohmann::json* outCommand = nullptr,
                                            float radius = 100.0f)
{
    if (!bot || !bot->GetMap()) return "";

    std::ostringstream oss;
    uint32 hpPct = bot->GetMaxHealth() ? uint32((100.0 * bot->GetHealth()) / bot->GetMaxHealth()) : 100;

    // 1. Survival first: it overrides everything else.
    if (bot->IsInCombat() && hpPct <= 30)
    {
        std::string spellName;
        if (uint32 defensive = FindUsableDefensive(bot, spellName))
        {
            oss << "SITUATION: You are at " << hpPct << "% health and in combat. You will die if you "
                << "keep attacking. Cast " << spellName << " (spell " << defensive << ") now, or move "
                << "away to disengage. Do not attack this turn.\n";

            if (outCommand && outCommand->is_null())
                *outCommand = nlohmann::json{{"type", "spell"},
                                             {"params", {{"spellid", defensive}}}};
        }
        else
        {
            oss << "SITUATION: You are at " << hpPct << "% health and in combat with no defensive "
                << "ability ready. Move away to disengage rather than trading more damage.\n";
        }
    }

    // 1b. Being attacked with nothing selected.
    //
    // This was the single largest gap in the assessment, and it stayed hidden
    // for a long time because the test hook adopted whichever bots came first,
    // which in a 500-bot world means bots idling in towns. Once TestBotFilter
    // could restrict the sample to bots actually fighting, the problem was
    // immediate: replaying six real combat situations from the live server, bots
    // under attack chose move_to 18/18 and attacked their attacker 0/18.
    //
    // The combat summary does name the attacker, near the top of the prompt, but
    // that position is ignored. Stating it here, where the assessment is read,
    // takes it to 18/18.
    //
    // hpPct > 30 keeps this out of the way of the survival branch above, which
    // has already told a badly hurt bot to defend or disengage. GetVictim() means
    // a bot already swinging at something is left alone.
    if (bot->IsInCombat() && hpPct > 30 && !bot->GetVictim())
    {
        Creature* attacker = nullptr;
        for (auto const& pair : bot->GetMap()->GetCreatureBySpawnIdStore())
        {
            Creature* c = pair.second;
            if (!c) continue;
            if (c->GetVictim() == bot) { attacker = c; break; }
        }

        if (attacker)
        {
            uint32 aguid = attacker->GetGUID().GetCounter();
            oss << "SITUATION: " << attacker->GetName() << " (guid " << aguid
                << ") is attacking you and is " << uint32(bot->GetDistance(attacker))
                << " yards away. Fight back: attack guid " << aguid
                << " now. Do not move away.\n";

            if (outCommand && outCommand->is_null())
                *outCommand = nlohmann::json{{"type", "attack"},
                                             {"params", {{"attack_guid", aguid}}}};
        }
    }

    // 2. Things already in reach, so moving is wasted effort.
    //
    // Measured: with a lootable corpse 3 yards away the model chose move_to or
    // attack 12/12, and with a completed quest and its giver at 1 yard it chose
    // move_to 12/12. The MOVEMENT block is the loudest thing in the prompt and
    // biases everything toward movement unless the alternative is spelled out.
    Creature* nearestLoot = nullptr;      float lootDist = 6.0f;
    Creature* nearestTurnIn = nullptr;    float turnInDist = 6.0f;
    Creature* nearestQuestGiver = nullptr; float nearestDist = 6.0f;

    // Quest objective in sight. Kept separate because, unlike the others, it is
    // not an "already in reach" case: it is what the bot should be doing when
    // nothing else needs attention.
    Creature* nearestQuestTarget = nullptr; float questTargetDist = 40.0f;
    std::string questTargetTitle;

    for (auto const& pair : bot->GetMap()->GetCreatureBySpawnIdStore())
    {
        Creature* c = pair.second;
        if (!c) continue;
        if (!bot->IsWithinDistInMap(c, radius)) continue;
        float dist = bot->GetDistance(c);

        // Lootable corpse the bot has rights to.
        if (c->isDead())
        {
            if (dist < lootDist && c->hasLootRecipient() &&
                (c->GetLootRecipient() == bot ||
                 (c->GetLootRecipientGroup() && bot->GetGroup() == c->GetLootRecipientGroup())))
            {
                nearestLoot = c;
                lootDist = dist;
            }
            continue;
        }

        // Is this creature an objective of an active quest? Same test the
        // visible-entity list uses to print "[QUEST TARGET - <quest>]".
        if (dist < questTargetDist && c->IsAlive() &&
            bot->IsWithinLOSInMap(c) && bot->IsValidAttackTarget(c))
        {
            for (auto const& qs : bot->getQuestStatusMap())
            {
                if (qs.second.Status != QUEST_STATUS_INCOMPLETE) continue;
                Quest const* q = sObjectMgr->GetQuestTemplate(qs.first);
                if (!q) continue;

                bool matched = false;
                for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                {
                    if (q->RequiredNpcOrGo[i] <= 0) continue;
                    if (q->RequiredNpcOrGo[i] != (int32)c->GetEntry()) continue;
                    if (bot->GetReqKillOrCastCurrentCount(qs.first, q->RequiredNpcOrGo[i])
                            < q->RequiredNpcOrGoCount[i])
                    {
                        matched = true;
                        break;
                    }
                }

                if (matched)
                {
                    nearestQuestTarget = c;
                    questTargetDist = dist;
                    questTargetTitle = q->GetTitle();
                    break;
                }
            }

            // Collection quests: the creature is not named by any quest, it
            // simply drops what a quest wants.
            //
            // RequiredNpcOrGo only covers kill and interact objectives, and those
            // are the minority. Of 998 incomplete quests held by online bots, 304
            // need an NPC and 668 need items, so two thirds of all quest progress
            // had no targeting at all: a bot carrying "collect 10 hides" would
            // stand beside the thing that drops them with nothing saying so.
            //
            // Asked once per creature rather than once per quest, because
            // HaveQuestLootForPlayer is a question about the player, not about one
            // quest -- it walks the creature's loot template, follows reference
            // entries, and checks every active quest itself. Putting it inside the
            // per-quest loop would have attributed the drop to whichever quest the
            // loop happened to be on, which is a wrong title on a correct action.
            if (!nearestQuestTarget || questTargetDist > dist)
            {
                if (CreatureTemplate const* ct = c->GetCreatureTemplate())
                {
                    if (ct->lootid && LootTemplates_Creature.HaveQuestLootForPlayer(ct->lootid, bot))
                    {
                        nearestQuestTarget = c;
                        questTargetDist = dist;
                        questTargetTitle = "one of your collection quests";
                    }
                }
            }
        }

        if (!c->IsQuestGiver()) continue;

        // Does this giver have one of our completed quests?
        bool hasTurnIn = false;
        QuestRelationBounds involved = sObjectMgr->GetCreatureQuestInvolvedRelationBounds(c->GetEntry());
        for (auto itr = involved.first; itr != involved.second; ++itr)
        {
            if (bot->GetQuestStatus(itr->second) == QUEST_STATUS_COMPLETE)
            {
                hasTurnIn = true;
                break;
            }
        }

        // Does it actually have a quest this bot could accept right now?
        //
        // IsQuestGiver() is only the permanent NPC flag: every quest giver in the
        // world carries it forever, whether or not it has anything left for this
        // particular bot. Announcing one as "a quest giver already within reach"
        // on that basis alone promises something the executor will refuse --
        // InteractWithQuestGiver() accepts a quest only when the status is
        // QUEST_STATUS_NONE and both CanTakeQuest and CanAddQuest pass, and
        // otherwise falls through to gossip and returns false, which surfaces to
        // the bot as "interact did not execute".
        //
        // The conditions below deliberately mirror InteractWithQuestGiver so the
        // situation assessment and the code that carries it out agree.
        bool hasOffer = false;
        QuestRelationBounds offers = sObjectMgr->GetCreatureQuestRelationBounds(c->GetEntry());
        for (auto itr = offers.first; itr != offers.second; ++itr)
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(itr->second);
            if (!quest) continue;
            if (bot->GetQuestStatus(itr->second) != QUEST_STATUS_NONE) continue;
            if (bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false))
            {
                hasOffer = true;
                break;
            }
        }

        if (hasTurnIn && dist < turnInDist) { nearestTurnIn = c; turnInDist = dist; }
        else if (!hasTurnIn && hasOffer && dist < nearestDist) { nearestQuestGiver = c; nearestDist = dist; }
    }

    if (nearestLoot)
    {
        oss << "SITUATION: There is a lootable corpse within reach (guid "
            << nearestLoot->GetGUID().GetCounter() << ", " << uint32(lootDist)
            << " yards). Loot it now. Do not move, and do not attack anything else first.\n";

        if (outCommand && outCommand->is_null())
            *outCommand = nlohmann::json{{"type", "loot"}, {"params", nlohmann::json::object()}};
    }

    if (nearestTurnIn)
    {
        oss << "SITUATION: " << nearestTurnIn->GetName() << " (guid "
            << nearestTurnIn->GetGUID().GetCounter() << ") has your completed quest and is within reach at "
            << uint32(turnInDist) << " yards. Turn the quest in now. You do not need to move.\n";

        if (outCommand && outCommand->is_null())
            *outCommand = nlohmann::json{{"type", "interact"},
                                         {"params", {{"guid", nearestTurnIn->GetGUID().GetCounter()}}}};
    }
    else if (nearestQuestGiver)
    {
        oss << "SITUATION: " << nearestQuestGiver->GetName() << " (guid "
            << nearestQuestGiver->GetGUID().GetCounter() << ") is a quest giver already within reach at "
            << uint32(nearestDist) << " yards. You do not need to move to reach it. Interact with it.\n";

        if (outCommand && outCommand->is_null())
            *outCommand = nlohmann::json{{"type", "interact"},
                                         {"params", {{"guid", nearestQuestGiver->GetGUID().GetCounter()}}}};
    }

    // 4. Nothing in reach needs doing: go make quest progress.
    //
    // The prompt already carried "*** QUEST TARGETS AVAILABLE! Attack ONLY the
    // LIVING creatures marked with [QUEST TARGET] ***", and the entity list
    // already tagged them, but that warning sits at ~18% depth in the prompt and
    // is ignored there like every other instruction outside this block. Measured
    // on real captured prompts with a tagged quest target visible:
    //
    //   warning at 18% depth (what production shipped)   attacked it   0/16
    //   the same instruction as a SITUATION line here    attacked it  16/16
    //
    // Emitted last on purpose. A corpse at the bot's feet, a completed quest to
    // hand in, or a giver with work available are all "already in reach" and
    // cost no travel, so they outrank walking off to fight something. The
    // !IsInCombat() guard leaves the branches above to deal with a fight that is
    // already happening rather than sending the bot after a different target.
    if (oss.str().empty() && nearestQuestTarget && !bot->IsInCombat())
    {
        oss << "SITUATION: " << nearestQuestTarget->GetName() << " (guid "
            << nearestQuestTarget->GetGUID().GetCounter() << ") is a target for your quest '"
            << questTargetTitle << "' and is " << uint32(questTargetDist)
            << " yards away. Attack guid " << nearestQuestTarget->GetGUID().GetCounter()
            << " now to make progress.\n";

        if (outCommand && outCommand->is_null())
            *outCommand = nlohmann::json{{"type", "attack"},
                                         {"params", {{"attack_guid", nearestQuestTarget->GetGUID().GetCounter()}}}};
    }

    std::string out = oss.str();
    return out.empty() ? out : out + "\n";
}

static std::string BuildBotPrompt(Player* bot)
{
    PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
    if (!botAI) return "";

    AreaTableEntry const* botCurrentArea = botAI->GetCurrentArea();
    AreaTableEntry const* botCurrentZone = botAI->GetCurrentZone();

    std::vector<std::string> groupInfo = GetGroupStatus(bot);

    std::string botName             = bot->GetName();
    uint32_t botLevel               = bot->GetLevel();
    uint8_t botGenderByte           = bot->getGender();
    std::string botAreaName         = botCurrentArea ? botAI->GetLocalizedAreaName(botCurrentArea): "UnknownArea";
    std::string botZoneName         = botCurrentZone ? botAI->GetLocalizedAreaName(botCurrentZone): "UnknownZone";
    std::string botMapName          = bot->GetMap() ? bot->GetMap()->GetMapName() : "UnknownMap";
    std::string botClass            = botAI->GetChatHelper()->FormatClass(bot->getClass());
    std::string botRace             = botAI->GetChatHelper()->FormatRace(bot->getRace());
    std::string botGender           = (botGenderByte == 0 ? "Male" : "Female");
    std::string botFaction          = (bot->GetTeamId() == TEAM_ALLIANCE ? "Alliance" : "Horde");
    std::string botGroupStatus      = (bot->GetGroup() ? "In a group" : "Solo");
    uint32_t botGold                = bot->GetMoney() / 10000;
    

    std::ostringstream oss;
    oss << "Bot state summary:\n";
    oss << "Name: " << botName << "\n";
    oss << "Level: " << botLevel << "\n";
    oss << "Class: " << botClass << "\n";
    oss << "Race: " << botRace << "\n";
    oss << "Gender: " << botGender << "\n";
    oss << "Faction: " << botFaction << "\n";
    oss << "Gold: " << botGold << "\n";
    oss << "Area: " << botAreaName << "\n";
    oss << "Zone: " << botZoneName << "\n";
    oss << "Map: " << botMapName << "\n";
    oss << "Position: " << bot->GetPositionX() << " " << bot->GetPositionY() << " " << bot->GetPositionZ() << "\n";

    oss << GetCombatSummary(bot) << "\n\n";

    // The situation assessment used to sit here, high in the prompt, on the
    // theory that it should be read before the movement instructions. Measured
    // against real prompts captured from the live server, that theory is wrong:
    // see the block appended at the very end of this function.

    // Result of the previous action. Placed before the command history so the
    // model reads the failure and its cause together, rather than seeing a
    // command it issued with no indication that it did not work.
    {
        std::string outcome = GetActionOutcome(bot);
        if (!outcome.empty())
        {
            oss << "YOUR LAST ACTION FAILED: " << outcome << "\n";
            oss << "Do not repeat it unchanged. Pick a different action or target.\n\n";
        }
    }

    // Personality used to be emitted here, near the top. It now goes at the end
    // of the prompt instead; see the block just before the situation assessment.

    // Destination candidates: computed here (world thread), stored for the
    // reply-handling thread, and listed so the model knows what each index is.
    {
        uint64_t destKey = bot->GetGUID().GetRawValue();
        std::vector<BotDestination> dests;

        // Reuse while the bot is within 15 yards of where the list was built and
        // it is under 10 seconds old. Beyond either, the visible objects and
        // reachable points have genuinely changed.
        constexpr uint32 kDestTtlMs = 10000;
        constexpr float  kDestMoveTolerance = 15.0f;

        bool rebuild = true;
        {
            std::lock_guard<std::mutex> lock(g_botDestinationsMutex);
            auto metaIt = g_botDestinationsMeta.find(destKey);
            auto listIt = g_botDestinations.find(destKey);

            if (metaIt != g_botDestinationsMeta.end() && listIt != g_botDestinations.end() &&
                getMSTimeDiff(metaIt->second.builtAtMs, getMSTime()) < kDestTtlMs)
            {
                float dx = bot->GetPositionX() - metaIt->second.x;
                float dy = bot->GetPositionY() - metaIt->second.y;
                float dz = bot->GetPositionZ() - metaIt->second.z;
                if ((dx * dx + dy * dy + dz * dz) < (kDestMoveTolerance * kDestMoveTolerance))
                {
                    dests = listIt->second;
                    rebuild = false;
                }
            }
        }

        if (rebuild)
        {
            dests = BuildDestinationCandidates(bot);
            std::lock_guard<std::mutex> lock(g_botDestinationsMutex);
            g_botDestinations[destKey] = dests;
            g_botDestinationsMeta[destKey] = { getMSTime(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ() };
        }
        if (!dests.empty())
        {
            oss << "ATTACKING: to attack, use params {\"attack_guid\": G}. Only creatures you "
                   "may actually attack appear there; friendly and neutral NPCs do not.\n";
            oss << "MOVEMENT: to move, use command type \"move_to\" with params "
                   "{\"destination_index\": N} choosing N from this list. These are the "
                   "ONLY valid destinations; they are already verified reachable.\n";
            for (size_t i = 0; i < dests.size(); ++i)
            {
                oss << " [" << i << "] " << dests[i].label << "\n";
            }
            oss << "\n";
        }
    }

    oss << "Your known spells:\n" << GetBotSpellInfo(bot) << "\n\n";

    // The bot's own role, plus what that role means in group content. Combined
    // with the per-member role tags above, this is what lets a five-bot party
    // behave like a party instead of five independent actors.
    oss << "Your group role: " << GroupRoleTag(bot) << "\n";
    if (bot->GetGroup())
    {
        if (PlayerbotAI::IsTank(bot))
            oss << "AS TANK: hold enemy attention, pull for the group, and keep enemies off healers and DPS.\n";
        else if (PlayerbotAI::IsHeal(bot))
            oss << "AS HEALER: keep group members alive. Prioritise the lowest HP% member. Heal before you attack.\n";
        else
            oss << "AS DPS: attack what the tank is fighting. Do not pull new enemies. Assist allies under attack.\n";
    }
    oss << "Group status: " << botGroupStatus << "\n";
    if (!groupInfo.empty()) {
        oss << "Group members:\n";
        for (const auto& entry : groupInfo) oss << " - " << entry << "\n";
    }

    oss << GetDetailedQuestInfo(bot) << "\n";

    std::vector<std::string> losLocs = GetVisibleLocations(bot);
    std::vector<std::string> wps = GetNearbyWaypoints(bot);

    if (!losLocs.empty()) {
        oss << "Visible locations/objects in line of sight:\n";
        for (const auto& entry : losLocs) oss << " - " << entry << "\n";
        
        // Check for critical priorities and add warnings
        bool hasEnemies = false;
        bool hasNeutrals = false;
        bool hasQuestTargets = false;
        bool hasQuestTurnIns = false;
        bool hasLootableCorpses = false;
        bool hasDeadCreatures = false;
        
        for (const auto& entry : losLocs) {
            if (entry.find("ENEMY:") != std::string::npos && entry.find("DEAD") == std::string::npos) {
                hasEnemies = true; // Only count living enemies
            }
            if (entry.find("NEUTRAL:") != std::string::npos && entry.find("DEAD") == std::string::npos) {
                hasNeutrals = true; // Only count living neutrals
            }
            if (entry.find("[QUEST TARGET") != std::string::npos) {
                hasQuestTargets = true;
            }
            if (entry.find("[QUEST GIVER - TURN IN READY]") != std::string::npos) {
                hasQuestTurnIns = true;
            }
            if (entry.find("DEAD") != std::string::npos) {
                hasDeadCreatures = true;
                if (entry.find("LOOTABLE") != std::string::npos) {
                    hasLootableCorpses = true;
                }
            }
        }
        
        // Priority warnings in order of importance
        if (hasQuestTurnIns) {
            oss << "*** HIGHEST PRIORITY: QUEST TURN-INS AVAILABLE! Find NPCs marked with [QUEST GIVER - TURN IN READY] immediately! ***\n";
        }
        if (hasLootableCorpses) {
            oss << "*** CRITICAL: DEAD CREATURES TO LOOT! Use 'loot' command on ALL creatures marked 'DEAD' or 'DEAD (LOOTABLE)' - NEVER attack dead creatures! ***\n";
        }
        if (hasQuestTargets) {
            oss << "*** QUEST TARGETS AVAILABLE! Attack ONLY the LIVING creatures marked with [QUEST TARGET] to complete your objectives! ***\n";
        }
        if (hasEnemies) {
            oss << "*** WARNING: LIVING ENEMIES ARE VISIBLE! You should attack LIVING enemies for XP and to defend yourself! ***\n";
        }
        if (hasNeutrals && !hasQuestTargets) {
            oss << "*** NEUTRAL CREATURES VISIBLE: These may be needed for quest objectives! Check if they are LIVING and attack if needed for quests! ***\n";
        }
        if (hasDeadCreatures) {
            oss << "*** IMPORTANT: ANY DEAD CREATURES MUST BE LOOTED, NOT ATTACKED! Use loot command for all creatures with 'DEAD' status! ***\n";
        }
    }

    if (!wps.empty()) {
        oss << "Nearby navigation waypoints:\n";
        for (const auto& entry : wps) oss << " - " << entry << "\n";
    }

    std::vector<std::string> nearbyPlayers = GetVisiblePlayers(bot);
    if (!nearbyPlayers.empty()) {
        oss << "Visible players in area:\n";
        for (const auto& entry : nearbyPlayers) oss << " - " << entry << "\n";
    }

    if (!losLocs.empty() || !wps.empty()) {
        oss << "You must select one of these locations or waypoints to move to, interact with, accept or turn in quests, attack, loot, or any other action or choose a new unexplored spot.\n";
        oss << "COORDINATE CALCULATION RULES:\n";
        oss << " - YOUR POSITION: Use your current Position coordinates as reference point for all calculations\n";
        oss << " - TO MOVE TO TARGETS: Use their exact 'Position: X Y Z' coordinates OR calculate closer positions\n";
        oss << " - TO MOVE CLOSER: Calculate coordinates 70% of the way between your position and target\n";
        oss << " - TO EXPLORE: Use waypoint coordinates from navigation list OR calculate new exploration points\n";
        oss << " - DISTANCE THRESHOLDS: <5.0=attack/interact directly, >15.0=move closer using calculated coordinates\n";
        oss << " - COORDINATE MATH: You can add/subtract 5-20 units from any position to create tactical positioning\n";
        oss << "IMPORTANT: You can ONLY attack creatures/NPCs that are listed above in the visible locations. If your quest requires creatures that are NOT visible, you must move to find them using waypoints or exploration.\n";
    }

    oss << FormatPlayerMessagesPromptSegment(bot);

    std::vector<std::string> cmdHist = GetBotCommandHistory(bot);

    std::vector<std::string> reasoningHist = GetBotReasoningHistory(bot);


    if (!cmdHist.empty() && !reasoningHist.empty())
    {
        oss << "Last 5 commands and their reasoning (most recent at the bottom):\n";
        for (size_t i = 0; i < cmdHist.size() && i < reasoningHist.size(); ++i)
        {
            oss << " - Command: " << cmdHist[i] << "\n";
            oss << "   Reasoning: " << reasoningHist[i] << "\n";
        }
        oss << "\nIMPORTANT: Look at your command history above! If you keep using move_to commands to the same location, switch to interact commands instead. If you keep trying to interact with the same NPC unsuccessfully, move away to find enemies or other NPCs.\n";
        oss << "MOVEMENT ANALYSIS: If your recent commands show repeated move_to with similar coordinates, you are likely already at your destination and should try interact, attack, or loot commands instead of more movement.\n";
    }

    if (g_EnableOllamaBotBuddyDebug)
    {
        std::string safeSnapshot = EscapeBracesForFmt(oss.str());
        LOG_INFO("server.loading", "[OllamaBotBuddy] Bot Snapshot for '{}': {}", botName, safeSnapshot);
    }

    oss << R"(You are an AI-controlled bot in World of Warcraft. Your task is to follow these strict rules and reply only with the listed acceptable commands:

    Primary goal: Level to 80 and equip the best gear. Prioritize combat, questing and quest givers that have available quests, talking to other players and efficient progression. If no available quests or viable enemies are nearby, turn in quests, explore for new quests, dungeons, raids, professions, or gold opportunities.

    SURVIVAL AND IMMEDIATE THREATS (HIGHEST PRIORITY):
    - If you are taking damage and not in combat with a target, IMMEDIATELY move away from your current position
    - If you see ENEMY creatures in your visible list and you're not fighting anything, ATTACK the nearest enemy immediately
    - DO NOT STAND ON CAMP FIRES or other environmental hazards - they cause damage
    - If your HP is dropping and you're not in combat, move to a safe location immediately
    - If you're under attack by enemies, prioritize combat over everything else

    QUEST PRIORITIZATION (HIGH PRIORITY):
    - If you have any quests marked READY TO TURN IN, that is your TOP PRIORITY - find the quest giver immediately
    - For incomplete quests, read the objectives carefully and focus on completing them:
      * If you need to kill creatures, prioritize those specific creatures over random enemies
      * If you need to collect items, look for the sources of those items
      * If you need to interact with objects, find and use those objects
      * If objectives show COMPLETE, that part is done - focus on incomplete objectives
    - When you see quest objectives that need specific creatures or items, prioritize those targets over random combat
    - Quest completion gives significant XP - completing quests is more efficient than random grinding

    CRITICAL QUEST BEHAVIOR:
    - NEVER waste time sitting at NPCs that have no available quests for you
    - If an NPC doesn't have "[QUEST GIVER - TURN IN READY]" or "[QUEST GIVER - QUESTS AVAILABLE]" tags, DO NOT prioritize them unless you have no other options
    - If you tried to interact with an NPC and nothing happened, that means they have no quests - MOVE AWAY IMMEDIATELY and find something else to do
    - Look at your command history - if you keep trying the same quest giver repeatedly, STOP and go elsewhere

    NPC INTERACTION DECISION LOGIC:
    - If you see an NPC within 15 yards with "[QUEST GIVER - TURN IN READY]" or "[QUEST GIVER - QUESTS AVAILABLE]" tags: USE INTERACT COMMAND
    - If you see such an NPC beyond 15 yards: USE MOVE_TO COMMAND to get closer first
    - ONLY interact with NPCs that have useful tags: [QUEST GIVER - TURN IN READY], [QUEST GIVER - QUESTS AVAILABLE], [VENDOR], [TRAINER], [FLIGHT MASTER], [INNKEEPER], [BANKER], [AUCTIONEER]
    - NEVER interact with generic friendly NPCs that have no useful tags - they are a waste of time
    - If you see a friendly NPC with no useful tags, IGNORE IT completely and focus on combat or exploration
    - If your last action was to interact with an NPC but you're still in the same position, that NPC was useless - find enemies to fight or new areas to explore

    COMBAT TARGETING AND POSITIONING:
    - ALWAYS select your target properly before attacking using the attack command
    - If you're too far from your target, MOVE CLOSER first before trying to attack
    - MELEE fighters must get within 5 yards of the target before attacking
    - RANGED fighters should maintain 6-25 yard distance from targets
    - If you're a MELEE fighter and the target is far away, use move_to command to get closer first
    - If you're a RANGED fighter and too close (distance < 6), move away before attacking

    QUEST TARGET HUNTING:
    - Look at your quest objectives and identify what creatures/items you need
    - Check your "Visible locations/objects" list to see if those creatures are currently visible
    - If quest target creatures ARE visible: attack them immediately (use their GUID from the visible list)
    - If quest target creatures are NOT visible: move to a waypoint or new area to search for them
    - NEVER try to attack creatures that aren't in your current visible list - move to find them first
    - If no quest targets are available, attack any hostile creatures visible for XP while searching

    COMBAT RULES:
    - NEVER ATTACK DEAD CREATURES: If a creature is marked as DEAD or DEAD (LOOTABLE), use the loot command instead of attack - this is CRITICAL
    - DEAD CREATURES = LOOT ONLY: Any creature with "DEAD" in its status should ONLY be looted, NEVER attacked
    - QUEST TARGET PRIORITY: Even for quest objectives, if the required creature is DEAD, use loot command instead of attack command
    - If you or a player in your group are under attack, IMMEDIATELY prioritize defense. Attack the enemy targeting you or your group, or escape if the enemy is much higher level.
    - During combat, do NOT disengage or move away unless your HP is low or the enemy is significantly stronger.
    - POSITIONING IS CRITICAL: Read your combat summary carefully to understand your role:
      * MELEE FIGHTERS: Must be within melee range (distance < 5). If you see TOO FAR FOR MELEE, move closer before attacking.
      * RANGED FIGHTERS: Maintain optimal distance (5-25 yards). If you see TOO CLOSE - NEED TO BACK AWAY, move away first. If you see TOO FAR FOR SPELLS, move closer.
      * Pay attention to range indicators: IN MELEE RANGE, GOOD RANGED POSITION, etc.
    - When choosing a target, move toward them if not in range. Use 'attack' only once you're within proper combat distance.
    - If you're too close to your target (distance <= 0.15) then move away before attacking again.
    - DO NOT TRY TO ATTACK OR DEFEND FROM CREATURES TAGGED AS DEAD - USE LOOT COMMAND INSTEAD.
    - BE AGGRESSIVE, killing things around your level grants you XP to level up. Attack monsters nearby to help level up.
    - QUEST CREATURES PRIORITY: Always attack creatures needed for your quest objectives, regardless of their faction (hostile, neutral, or friendly)
    - If no quest target creatures are visible, prioritize attacking hostile creatures for XP and safety
    - NEUTRAL CREATURES: Attack neutral creatures if they are needed for quest objectives or if they're aggressive toward you
    - Make sure you're using your spells, if you have the resource cost and the spell sounds like it would help in combat, use a spell command picking a logical target guid!
    - COMBAT TYPE AWARENESS: Your combat summary shows if you're a MELEE, RANGED, or HYBRID fighter. Use this to determine proper positioning and tactics.

    DECISION RULE (ABSOLUTE PRIORITY ORDER):
    1. SURVIVAL FIRST: If you're taking damage and not in combat, move away from environmental hazards immediately
    2. QUEST TURN-INS (ABSOLUTE HIGHEST PRIORITY): If ANY quest shows READY TO TURN IN status, IMMEDIATELY find the quest giver with [QUEST GIVER - TURN IN READY] tag - this takes priority over ALL combat, looting, and other activities
    3. LOOTING DEAD CREATURES (CRITICAL): If you see ANY creatures marked as DEAD or DEAD (LOOTABLE) in your visible list, use the loot command immediately - NEVER attack dead creatures, ALWAYS loot them for XP and items
    4. QUEST OBJECTIVES: For INCOMPLETE quests only, prioritize completing quest objectives over random combat - but ONLY attack LIVING creatures, never dead ones
    5. VISIBLE ENEMIES: If you see any LIVING ENEMY creatures in your visible list, attack them for XP - but ONLY if you have NO completed quests to turn in and NO dead creatures to loot
    - For incomplete quests, target the specific creatures or objects needed for quest objectives rather than random enemies
    - CRITICAL: You can ONLY interact with, attack, or move to objects/creatures that are listed in your Visible locations/objects section - NEVER try to attack or interact with creatures/NPCs that aren't currently visible
    - **GUID USAGE CRITICAL**: When using attack, interact, or spell commands, you MUST copy the exact GUID number from the visible locations list. DO NOT make up or guess GUID numbers!
    - EXAMPLE: If you see ENEMY: Kobold Vermin (guid: 604, Level: 1...), use exactly 604 as the GUID in your attack command
    - INVALID: Using made-up GUIDs like 1234, 5678, or any number not explicitly shown in your visible locations
    - VALID: Only use GUIDs that appear in parentheses after guid: in your visible locations list
    - If quest objectives require specific creatures that are NOT in your visible list, you must move to find them - use waypoints or explore new areas
    - Always choose the most effective single action to level up, complete quests, gain gear, or respond to threats.
    - MOVEMENT LOGIC: Before using move_to, check your current position and the target's distance:
      * Your current position is shown in "Position: X Y Z" in your bot state summary
      * Target distances are shown in your visible objects list as "Distance: X.X"
      * If Distance < 6.0, you're close enough to interact/attack - DON'T move closer
      * If you keep moving to the same coordinates, you're probably already there - try interact/attack instead
      * Look at your command history - if your last move_to didn't change your situation, try a different action
    - ANY other format or additional text reply is INVALID.
    - Base your decisions on the current game state, visible objects, group status, and your last 5 commands along with their reasoning. For example, if your previous command was to move and attack a target, and that target is still present and within range, your next action should likely be to execute an attack command.
    - DEAD CREATURE LOOTING: If you see a creature marked as DEAD (LOOTABLE) in your visible list, ALWAYS use the loot command to loot its body for XP and items - NEVER try to attack dead creatures
    - QUEST TARGET LOGIC:
      * CRITICAL: Check if creatures are ALIVE before attacking - NEVER attack dead creatures
      * If a quest target creature is DEAD or DEAD (LOOTABLE), use loot command instead of attack
      * First, check if the LIVING creatures you need for quest objectives are in your visible list - if yes, attack them
      * If quest target creatures are NOT visible, move to a waypoint or new area to search for them
      * If no LIVING quest targets are visible and no useful NPCs are available, attack any LIVING hostile creature in your visible list for XP
      * NEVER try to attack creatures that aren't in your current visible list - they don't exist in your current area
      * DEAD CREATURES ANYWHERE = LOOT ONLY, regardless of quest status
    - QUEST GIVER INTERACTION LOGIC: 
      * If you see an NPC within 15 yards with [QUEST GIVER - TURN IN READY] or [QUEST GIVER - QUESTS AVAILABLE] tags: USE INTERACT COMMAND immediately
      * If you see such an NPC beyond 15 yards: USE MOVE_TO COMMAND to get closer first
      * NEVER keep moving to the same quest giver if you're already close - switch to interact command
      * COMPLETELY IGNORE all other NPCs unless they have useful tags like [VENDOR], [TRAINER], [FLIGHT MASTER], [INNKEEPER], [BANKER], [AUCTIONEER]
      * NEVER interact with NPCs that have no quest tags, no useful service tags, or are just generic friendly NPCs
      * If you see an NPC with no available quests, IMMEDIATELY move away and find a different target
      * If your last command was to interact with a quest giver but you're still at the same location, that means the NPC had no quests - MOVE ELSEWHERE IMMEDIATELY
      * Do NOT repeatedly try to interact with the same quest giver - if it didn't work the first time, that NPC has no available quests for you
      * PRIORITIZE ENEMIES TO KILL over useless friendly NPCs - combat gives XP, talking to random NPCs does not
      * IF YOUR LAST COMMAND WAS move_to TO A QUEST GIVER AND YOU'RE NOW CLOSE TO THEM, YOUR NEXT COMMAND SHOULD BE interact
    - CRITICAL ENVIRONMENTAL SAFETY: If you are taking damage from environmental sources (like standing on campfires), IMMEDIATELY move to safety before doing anything else
    
    NAVIGATION AND COORDINATE CALCULATION:
    - **SMART COORDINATE CALCULATION**: You can calculate new coordinates based on your position and visible objects!
    - **YOUR CURRENT POSITION**: Always shown as "Position: X Y Z" in your bot state summary
    - **DISTANCE-BASED MOVEMENT RULES**:
      * Distance < 5.0: Close enough for melee attack/interact - DO NOT MOVE, use attack/interact command
      * Distance 5.0-15.0: Usually close enough for most actions, but may need positioning
      * Distance > 15.0: Too far - calculate coordinates to move closer
    - **COORDINATE CALCULATION METHODS**:
      * TO MOVE TO TARGET: Use target exact Position: X Y Z coordinates from visible list
      * TO MOVE CLOSER: Calculate coordinates between your position and target (move 70% of the way)
      * TO EXPLORE: Use waypoint coordinates from Node format (X, Y, Z)
      * TO ESCAPE DANGER: Calculate coordinates away from your current position (add/subtract 10-20 units)
      * TO POSITION FOR RANGED: Calculate coordinates 8-12 units away from target in any direction
    - **MOVEMENT CALCULATION EXAMPLES**:
      * Your Position: -8920.1 -140.2 82.1, Target Position: -8913.2 -133.5 81.7, Distance: 25.3
      * To move closer: Calculate midpoint or 70% distance: X = -8920.1 + ((-8913.2 - -8920.1) * 0.7) = -8915.3
      * Y = -140.2 + ((-133.5 - -140.2) * 0.7) = -135.5, Z = 82.1 + ((81.7 - 82.1) * 0.7) = 81.8
      * Result: move_to x: -8915.3, y: -135.5, z: 81.8
    - **POSITIONING LOGIC**:
      * MELEE FIGHTERS: Move to target's exact position for close combat
      * RANGED FIGHTERS: Move to position 8-12 units away from target (calculate offset from target position)
      * ESCAPE/SAFETY: Move 15-20 units away from current position in safe direction
    - **FORBIDDEN**: Never use completely random numbers like -1000, -200, -50 that have no relation to visible positions
    - If you're in a group, try to stay within 5-10 distance of another group member if you're not engaged in combat.
    - Do not move DIRECTLY on top of other players, creatures or objects, always maintain a distance to avoid collision issues.

    COMMUNICATION:
    - Be chatty only in the say field! Talk to other players, comment on things or people around you or your intentions and goals.
    - To make your character say something to players, put the message as a string in the top-level say field.
    - Make yourself seem as human as possible, ask players for help if you don't understand something or need help finding something or killing something or completing a quest. Ask a nearby real player and use their response in your reasoning.

    CRITICALLY IMPORTANT: Reply with EXACTLY and ONLY a single valid JSON object, no extra text, no comments, no code block formatting. Your JSON must be properly formatted with quotes around all strings:
    {
    \"command\": { \"type\": <string>, \"params\": { ... } },
    \"reasoning\": <string>,
    \"say\": <string>
    }

    Allowed type values and required params (ALL STRINGS MUST HAVE QUOTES):

    - \"move_to\": params = { \"x\": float, \"y\": float, \"z\": float }
    - \"attack\": params = { \"guid\": int }
    - \"interact\": params = { \"guid\": int }
    - \"spell\": params = { \"spellid\": int, \"guid\": int (omit if self-cast) }
    - \"loot\": params = { }
    - \"accept_quest\": params = { \"id\": int }
    - \"turn_in_quest\": params = { \"id\": int }
    - \"follow\": params = { }
    - \"stop\": params = { }

    \"reasoning\" must be a short natural-language explanation for why you chose this command (WITH QUOTES).
    \"say\" must be what your character would say in-game to players, or empty string if nothing is to be said (WITH QUOTES).

    **CRITICAL GUID REQUIREMENT**: For attack, interact, and spell commands, you MUST use the exact GUID numbers from your visible locations list. DO NOT make up numbers!

    **ABSOLUTE RULE: DEAD CREATURES = LOOT ONLY, NEVER ATTACK!**
    - If ANY creature has DEAD in its status description, use loot command ONLY
    - NEVER use attack command on dead creatures, even for quest objectives
    - Dead creatures give XP and items through looting, not attacking

    EXAMPLES (USE EXACT JSON FORMAT WITH QUOTES AND CALCULATED COORDINATES):
    {
    \"command\": { \"type\": \"move_to\", \"params\": { \"x\": -8913.2, \"y\": -133.5, \"z\": 81.7 } },
    \"reasoning\": \"Moving to Kobold Vermin's exact position -8913.2 -133.5 81.7 from visible list - distance 25.3 is too far to attack directly.\",
    \"say\": \"Moving closer to attack that Kobold.\"
    }
    {
    \"command\": { \"type\": \"move_to\", \"params\": { \"x\": -8915.3, \"y\": -135.5, \"z\": 81.8 } },
    \"reasoning\": \"Calculating position 70% of the way to Kobold. My position: -8920.1 -140.2 82.1, Target: -8913.2 -133.5 81.7. Calculated: -8915.3 -135.5 81.8\",
    \"say\": \"Moving strategically closer.\"
    }
    {
    \"command\": { \"type\": \"move_to\", \"params\": { \"x\": -8905.2, \"y\": -125.5, \"z\": 81.7 } },
    \"reasoning\": \"Positioning for ranged combat. Target at -8913.2 -133.5 81.7, calculating position 8 units away: -8905.2 -125.5 81.7\",
    \"say\": \"Getting into ranged position.\"
    }
    {
    \"command\": { \"type\": \"move_to\", \"params\": { \"x\": -8935.1, \"y\": -155.2, \"z\": 82.1 } },
    \"reasoning\": \"Escaping danger by moving 15 units away from my current position -8920.1 -140.2 82.1 to safety at -8935.1 -155.2 82.1\",
    \"say\": \"Moving to safety!\"
    }
    {
    \"command\": { \"type\": \"attack\", \"params\": { \"guid\": 604 } },
    \"reasoning\": \"Attacking Kobold Vermin GUID 604 - distance 4.2 is close enough for melee combat.\",
    \"say\": \"Attacking the Kobold!\"
    }
    {
    \"command\": { \"type\": \"move_to\", \"params\": { \"x\": -9123.4, \"y\": 267.8, \"z\": 73.2 } },
    \"reasoning\": \"Moving to waypoint Node #5 coordinates -9123.4 267.8 73.2 to explore for new quest targets.\",
    \"say\": \"Exploring a new area.\"
    }

    REMEMBER: NEVER REPLY WITH ANYTHING OTHER THAN A PROPERLY FORMATTED JSON OBJECT WITH QUOTES AROUND ALL STRINGS!!!

    SPEECH: your \"say\" is dialogue, not narration. Never describe what you are doing (\"Checking for quests\", \"Moving to the target\") -- the reasoning field is for that. Say something a person would type in chat: a greeting, a complaint, a joke, a question, trash talk, or nothing at all. Most of the time an empty string is right.
    )";

    // Situation assessment goes LAST, and this placement is load-bearing.
    //
    // It used to be near the top, reasoning that it should be read before the
    // movement instructions. Replaying 12 real prompts captured from the live
    // server shows that reasoning is backwards. The wording is identical in both
    // arms; only the position differs:
    //
    //   situation high in the prompt   stayed put   0/36
    //   situation at the end           stayed put  36/36
    //
    // In the failing arm the prompt says "X is a quest giver already within
    // reach at 0 yards. You do not need to move." and the model emits move_to
    // anyway, every single time. Suppressing the destination menu instead does
    // NOT fix it (0/12), so this is recency, not menu pressure.
    //
    // Earlier synthetic evals scored this 18/18 and missed the bug entirely,
    // because a short synthetic prompt has nothing to bury the line under. Real
    // prompts are ~24,000 characters. Test prompt changes against captured
    // prompts, not hand-written ones.
    // Personality, sourced from mod-ollama-chat's assignment so speech and
    // behaviour come from the same disposition.
    //
    // Emitted here rather than near the top, where it used to sit at about 1.2%
    // depth. That is the same position every other instruction in this module was
    // ignored at, and moving it is the only thing that has improved speech
    // variety. Replayed on two disjoint sets of captured prompts, 14 prompts by
    // 3 samples each:
    //
    //   persona near the top   distinct 53% and 46%, most-repeated line 6x and 8x
    //   persona at the end     distinct 82% and 82%, most-repeated line 3x and 2x
    //
    // The content changes character too: generic "do you have any quests for me"
    // gives way to lines that sound like somebody, e.g. "Got your back, mates!"
    // and "Let's get this over with." Command selection was identical in every
    // arm, so this moves speech without touching behaviour.
    //
    // Deliberately before the situation assessment, which stays last: that block
    // is load-bearing (0/66 to 66/66) and nothing should displace it.
    {
        std::string personalityKey = GetBotPersonalityKey(bot);
        if (!personalityKey.empty())
        {
            std::string traits = ActionTraitsFor(personalityKey);
            oss << "\nYOUR PERSONALITY: " << personalityKey << "\n";
            if (!traits.empty())
            {
                oss << "This shapes how you act, not just how you talk:\n";
                oss << " - " << traits << "\n";
            }
        }
    }

    oss << "\n" << BuildSituationAssessment(bot);

    return oss.str();
}

namespace
{
    struct OllamaBotState
    {
        std::atomic<bool> busy { false };
        time_t lastRequest { 0 };
    };
    std::unordered_map<uint64_t, OllamaBotState> ollamaBotStates;

    // Bots whose playerbots strategies we have cleared. Taking a bot over is
    // destructive: ClearStrategies() wipes its combat/non-combat/dead strategy
    // sets. Without tracking this we could never give them back, so a bot that
    // was once LLM-driven would enter a battleground or dungeon permanently
    // stripped of the AI that content depends on.
    std::unordered_set<uint64_t> ollamaTakenOver;

    // LLM replies waiting to be applied on the world thread.
    //
    // The HTTP call blocks for seconds so it runs on a detached thread, but
    // nothing there may touch game state: the bot can log out, be level-reset
    // by the bracket system, or the server can shut down mid-request. The
    // worker therefore only parks the raw reply here, keyed by guid, and
    // OnUpdate drains it on the world thread where the bot is re-resolved.
    struct OllamaPendingReply
    {
        ObjectGuid  botGuid;
        uint64_t    stateKey;
        std::string reply;
    };
    std::vector<OllamaPendingReply> ollamaPendingReplies;
    std::mutex ollamaPendingRepliesMutex;

    // Multi-step plans.
    //
    // Previously the model emitted one action per request with no memory of
    // intent, so it could not express "walk to the quest giver, then talk to
    // him, then take the quest" -- and with no feedback on whether the last
    // action worked it would re-issue the same step indefinitely.
    //
    // A plan is a short ordered list of steps executed one per decision tick.
    // The LLM is only consulted when a plan runs out or fails, which also cuts
    // inference cost roughly in proportion to plan length.
    struct OllamaBotPlan
    {
        std::string              intent;
        std::vector<std::string> steps;   // each a serialised command object
        size_t                   next { 0 };
    };
    std::unordered_map<uint64_t, OllamaBotPlan> ollamaBotPlans;

    // Speculative next plan, requested while the final step of the current plan
    // is still executing. Inference takes seconds; without this the bot stalls
    // for a full round trip between every plan. The speculation is that the last
    // step succeeds, so a failed final step discards the prefetch: it was built
    // against a world state that never happened.
    std::unordered_map<uint64_t, OllamaBotPlan> ollamaPrefetchedPlans;
}

// Hand a bot back to playerbots with its native strategies rebuilt.
static void ReleaseBotToPlayerbots(Player* bot)
{
    if (!bot) return;
    uint64_t guid = bot->GetGUID().GetRawValue();
    if (!ollamaTakenOver.count(guid)) return;

    if (PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot))
    {
        ai->ResetStrategies();

        // Logged so a real battleground session produces evidence that the
        // handoff fired, rather than us inferring it from a clean build.
        LOG_INFO("playerbots", "[OllamaBotBuddy] Released '{}' back to playerbots AI (bg={} arena={} lfg={})",
                 bot->GetName(),
                 bot->InBattleground() ? 1 : 0,
                 bot->InArena() ? 1 : 0,
                 bot->inRandomLfgDungeon() ? 1 : 0);
    }

    ollamaTakenOver.erase(guid);
}

std::string EscapeBracesForFmt(const std::string& input) {
    std::string output;
    output.reserve(input.size() * 2); // Avoid lots of reallocs

    for (char c : input) {
        if (c == '{' || c == '}') {
            output.push_back(c); // first brace
            output.push_back(c); // second brace
        } else {
            output.push_back(c);
        }
    }
    return output;
}

// Fire an LLM request for this bot. World thread only: it reads the bot's map
// to build the prompt and schema, then hands the blocking HTTP call to a worker.
static void FireLlmRequest(Player* bot, uint64_t guid, OllamaBotState& state)
{
    state.busy = true;
    state.lastRequest = time(nullptr);

    std::string prompt = BuildBotPrompt(bot);

    size_t destCount = 0;
    {
        std::lock_guard<std::mutex> lock(g_botDestinationsMutex);
        auto it = g_botDestinations.find(bot->GetGUID().GetRawValue());
        if (it != g_botDestinations.end()) destCount = it->second.size();
    }
    nlohmann::json actionSchema = BuildBotActionSchema(bot, destCount);

    ObjectGuid botGuid = bot->GetGUID();
    std::thread([botGuid, guid, prompt, actionSchema]() {
        std::string llmReply = QueryOllamaLLM(prompt, actionSchema);

        std::lock_guard<std::mutex> lock(ollamaPendingRepliesMutex);
        ollamaPendingReplies.push_back({botGuid, guid, std::move(llmReply)});
    }).detach();
}

// ---------------------------------------------------------------------------
// Human movement quirks.
//
// Bots path in dead straight lines and stand perfectly still, which is the
// single most obvious tell that a character is not a person. Real players
// bunny-hop while waiting, spin on the spot, and fidget with their facing.
//
// Two rules keep this safe:
//   * Only fires while the bot is idle (no active movement generator). Jumping
//     mid-path would call MotionMaster::Clear() and cancel whatever the bot was
//     actually doing.
//   * Never fires in combat, or in instanced content where positioning matters.
//
// Each bot gets a fixed quirk profile hashed from its guid, so an individual
// bot is consistently fidgety or consistently still rather than everyone
// behaving identically.
// ---------------------------------------------------------------------------
namespace
{
    struct HumanQuirk
    {
        uint8 hopChance;    // percent, per opportunity
        uint8 spinChance;
        uint8 glanceChance;
        uint8 emoteChance;
        uint32 minGapMs;    // personal rhythm, so they do not sync up
    };

    HumanQuirk QuirkFor(Player* bot)
    {
        uint32 seed = bot ? bot->GetGUID().GetCounter() : 0;
        seed ^= seed >> 16; seed *= 0x7feb352dU; seed ^= seed >> 15;

        switch (seed % 5)
        {
            case 0:  return { 40,  8, 20, 12, 2500 };   // hopper
            case 1:  return {  4,  6, 35, 25, 5000 };   // people-watcher
            case 2:  return { 18, 26, 16, 15, 3500 };   // spinner
            case 3:  return {  2,  2,  6,  6, 9000 };   // still, patient type
            default: return { 15, 10, 25, 20, 4000 };   // average
        }
    }

    // Idle social emotes. Weighted by hand: people wave and talk far more than
    // they roar or grovel.
    uint32 const kIdleEmotes[] =
    {
        EMOTE_ONESHOT_TALK, EMOTE_ONESHOT_TALK, EMOTE_ONESHOT_TALK,
        EMOTE_ONESHOT_WAVE, EMOTE_ONESHOT_WAVE,
        EMOTE_ONESHOT_POINT, EMOTE_ONESHOT_QUESTION, EMOTE_ONESHOT_EXCLAMATION,
        EMOTE_ONESHOT_LAUGH, EMOTE_ONESHOT_CHEER, EMOTE_ONESHOT_APPLAUD,
        EMOTE_ONESHOT_SALUTE, EMOTE_ONESHOT_BOW,
        EMOTE_ONESHOT_FLEX, EMOTE_ONESHOT_DANCE, EMOTE_ONESHOT_ROAR,
        EMOTE_ONESHOT_RUDE, EMOTE_ONESHOT_EAT, EMOTE_ONESHOT_KNEEL
    };

    std::unordered_map<uint64_t, uint32> g_nextQuirkTime;

    // Reactions to a real player walking past. Bots emote into empty space on
    // their own; noticing someone is what actually reads as a person. Same
    // faction gets a greeting, the other faction gets what the other faction
    // has always got.
    uint32 const kFriendlyEmotes[] =
    {
        EMOTE_ONESHOT_WAVE, EMOTE_ONESHOT_WAVE, EMOTE_ONESHOT_WAVE,
        EMOTE_ONESHOT_SALUTE, EMOTE_ONESHOT_BOW, EMOTE_ONESHOT_CHEER,
        EMOTE_ONESHOT_APPLAUD, EMOTE_ONESHOT_TALK, EMOTE_ONESHOT_POINT
    };
    uint32 const kHostileEmotes[] =
    {
        EMOTE_ONESHOT_RUDE, EMOTE_ONESHOT_RUDE,
        EMOTE_ONESHOT_ROAR, EMOTE_ONESHOT_FLEX,
        EMOTE_ONESHOT_LAUGH, EMOTE_ONESHOT_POINT
    };

    // Separate, longer cooldown so a bot does not spam the same passer-by.
    std::unordered_map<uint64_t, uint32> g_nextSocialTime;
}

static void ApplyHumanMovement(Player* bot)
{
    if (!bot || !bot->IsInWorld() || !bot->IsAlive()) return;

    // Bots only. This is called from a loop over every Player in the world,
    // which includes real people: without this check the fidget code grabs the
    // player's own character and spins it, which is exactly as alarming as it
    // sounds. WorldSession::IsBot() is the authoritative test.
    if (!bot->GetSession() || !bot->GetSession()->IsBot()) return;

    // Combat and battlegrounds used to be excluded outright, which made bots
    // motionless in exactly the places players move most. A player in a
    // battleground is never still: they hop, they shuffle, they emote at the
    // enemy across the flag room.
    //
    // Combat still restricts WHICH quirks may fire, because one of them is
    // genuinely unsafe there: SetFacingTo turns the bot away from its target,
    // which stops melee swings and breaks directional spells. Hops and emotes do
    // neither. The idle checks below already guarantee this never competes with a
    // chase or charge generator, since those are not IDLE_MOTION_TYPE.
    //
    // Measured, because it was not obvious: 88.6% of in-combat observations pass
    // that idle guard (163,843 sampled, 145,210 with IDLE_MOTION_TYPE). Bots in a
    // fight stand still roughly nine tenths of the time, so the combat branch is
    // very much reachable -- and that stillness is the thing being fixed.
    bool const inCombat = bot->IsInCombat();

    // Idle only: anything else means a movement generator owns this bot.
    if (bot->isMoving()) return;
    if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != IDLE_MOTION_TYPE) return;

    uint64_t guid = bot->GetGUID().GetRawValue();
    uint32 now = getMSTime();

    auto it = g_nextQuirkTime.find(guid);
    if (it != g_nextQuirkTime.end() && now < it->second) return;

    HumanQuirk const q = QuirkFor(bot);

    // Notice a real player nearby first: reacting to someone beats fidgeting.
    {
        // Not while fighting: this ends in SetFacingToObject, which is the very
        // thing that must not happen mid-combat.
        auto socialIt = g_nextSocialTime.find(guid);
        if (!inCombat && (socialIt == g_nextSocialTime.end() || now >= socialIt->second))
        {
            Player* nearby = nullptr;
            float nearest = 18.0f;

            for (auto const& itr : ObjectAccessor::GetPlayers())
            {
                Player* other = itr.second;
                if (!other || other == bot || !other->IsInWorld() || !other->IsAlive()) continue;
                // Real people only: bots greeting each other endlessly is noise.
                if (!other->GetSession() || other->GetSession()->IsBot()) continue;
                if (!bot->IsWithinDistInMap(other, nearest)) continue;
                if (!bot->IsWithinLOS(other->GetPositionX(), other->GetPositionY(), other->GetPositionZ())) continue;

                nearest = bot->GetDistance(other);
                nearby = other;
            }

            if (nearby)
            {
                g_nextSocialTime[guid] = now + 45000 + urand(0, 60000);

                bool hostile = bot->GetTeamId() != nearby->GetTeamId();
                uint32 const* table = hostile ? kHostileEmotes : kFriendlyEmotes;
                size_t count = hostile ? (sizeof(kHostileEmotes) / sizeof(kHostileEmotes[0]))
                                       : (sizeof(kFriendlyEmotes) / sizeof(kFriendlyEmotes[0]));

                // Face them, then emote: an emote aimed at nobody looks broken.
                bot->SetFacingToObject(nearby);
                bot->HandleEmoteCommand(table[urand(0, count - 1)]);

                g_nextQuirkTime[guid] = now + q.minGapMs;
                return;
            }
        }
    }

    // Challenge a nearby bot to a duel.
    //
    // Duelling outside the city gates is one of the most recognisable things on
    // a populated realm, and none of the social repertoire existed here at all.
    // This is entirely deterministic: pick a same-faction bot standing close by
    // and cast Duel at it. Everything else is already handled by code that is not
    // ours -- Spell::EffectDuel refuses a duel when either side is already in
    // one, when the target ignores the caster, or when the zone does not permit
    // duels, which is why this ends up happening at the gates rather than inside
    // a sanctuary. The target accepts through mod-playerbots' DuelStrategy, which
    // lives in the combat engine and so survives takeover, and which declines
    // below 90% health.
    if (g_OllamaDuelCooldownSeconds > 0 && !bot->duel)
    {
        static std::unordered_map<uint64_t, uint32> nextDuel;
        auto dit = nextDuel.find(guid);

        // First sight of this bot: give it a random position in the cycle rather
        // than letting it challenge immediately. Without this every bot is
        // eligible the moment the server comes up, and they all challenge at
        // once: measured 80 of 500 players duelling in the first minute after a
        // restart, decaying to about 10 only as the cooldowns spread themselves
        // out. Seeding the phase keeps it at the steady state from the start.
        if (dit == nextDuel.end())
        {
            nextDuel[guid] = now + urand(0, g_OllamaDuelCooldownSeconds * IN_MILLISECONDS);
        }
        else if (now >= dit->second)
        {
            Player* opponent = nullptr;
            for (auto const& itr : ObjectAccessor::GetPlayers())
            {
                Player* other = itr.second;
                if (!other || other == bot || !other->IsInWorld() || !other->IsAlive()) continue;
                // Bots only: challenging the player unprompted would be rude.
                if (!other->GetSession() || !other->GetSession()->IsBot()) continue;
                if (other->IsInCombat() || other->duel) continue;
                if (other->GetTeamId() != bot->GetTeamId()) continue;   // duels are same-faction
                if (!bot->IsWithinDistInMap(other, 8.0f)) continue;
                if (!bot->IsWithinLOS(other->GetPositionX(), other->GetPositionY(), other->GetPositionZ())) continue;
                opponent = other;
                break;
            }

            if (opponent)
            {
                uint32 const base = g_OllamaDuelCooldownSeconds * IN_MILLISECONDS;
                nextDuel[guid] = now + base + urand(0, base);

                bot->SetFacingToObject(opponent);
                bot->CastSpell(opponent, 7266, true);   // Duel

                if (g_EnableOllamaBotBuddyDebug)
                {
                    LOG_INFO("server.loading", "[OllamaBotBuddy] {} challenges {} to a duel",
                             bot->GetName(), opponent->GetName());
                }

                g_nextQuirkTime[guid] = now + q.minGapMs;
                return;
            }
        }
    }

    g_nextQuirkTime[guid] = now + q.minGapMs + urand(0, q.minGapMs);

    uint32 roll = urand(0, 99);

    if (inCombat)
    {
        // Fighting: shuffle sideways, hop, or emote -- never turn.
        //
        // Measured before writing this: bots in combat sit on IDLE_MOTION_TYPE
        // for 88.6% of observations. They are not repositioning at all, which is
        // what makes a fight look scripted. Players never stand still: they
        // strafe to make themselves harder to hit and to move around obstacles.
        //
        // This is deliberately plain code rather than a prompt. Whether a step is
        // safe is a lookup, not a judgement: the server knows the target's
        // position, the bot's range band, line of sight, and whether a cast is in
        // progress.
        if (Unit* victim = bot->GetVictim())
        {
            // Never while casting. Movement cancels a cast bar, so a strafing
            // caster would simply stop doing damage.
            bool const casting = bot->IsNonMeleeSpellCast(false);

            if (g_OllamaCombatStrafe && !casting && urand(0, 1))
            {
                PlayerbotAI* pai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
                bool const melee = pai ? pai->IsMelee(bot) : true;

                // Stay inside the band the bot actually fights from. Melee has to
                // remain in swing range; a caster wants to keep its distance and
                // must not drift out of spell range.
                float const dist = bot->GetDistance(victim);
                float const maxBand = melee ? 4.0f : (pai ? pai->GetRange("spell") : 25.0f);
                float const minBand = melee ? 0.0f : 8.0f;

                if (dist <= maxBand && dist >= minBand)
                {
                    // Step perpendicular to the line to the target, so distance
                    // barely changes: that is a strafe, not an approach or a
                    // retreat.
                    float const toTarget = bot->GetAngle(victim);
                    float const side = urand(0, 1) ? (toTarget + float(M_PI) / 2.0f)
                                                   : (toTarget - float(M_PI) / 2.0f);
                    float const step = frand(2.0f, 4.0f);

                    float nx = bot->GetPositionX() + std::cos(side) * step;
                    float ny = bot->GetPositionY() + std::sin(side) * step;
                    float nz = bot->GetPositionZ();
                    bot->UpdateAllowedPositionZ(nx, ny, nz);

                    // Measured: ~93% of attempts clear all three checks (tried
                    // 132, fired 125 over four minutes), so these are guards, not
                    // a filter that quietly disables the feature.
                    float const newDist = victim->GetDistance(nx, ny, nz);
                    bool const bandOk  = (newDist <= maxBand && newDist >= minBand);
                    bool const losOk   = victim->IsWithinLOS(nx, ny, nz);

                    PathGenerator path(bot);
                    path.CalculatePath(nx, ny, nz, false);
                    bool const pathOk = !(path.GetPathType() & PATHFIND_NOPATH);

                    if (bandOk && losOk && pathOk)
                    {
                        bot->GetMotionMaster()->MovePoint(0, nx, ny, nz);
                        return;
                    }
                }
            }
        }

        if (urand(0, 3))
            bot->GetMotionMaster()->MoveJump(bot->GetPositionX(), bot->GetPositionY(),
                                             bot->GetPositionZ(), 0.1f, 7.0f);
        else
        {
            constexpr size_t kCombatEmoteCount = sizeof(kIdleEmotes) / sizeof(kIdleEmotes[0]);
            bot->HandleEmoteCommand(kIdleEmotes[urand(0, kCombatEmoteCount - 1)]);
        }
        return;
    }

    if (roll < q.hopChance)
    {
        // Hop on the spot. Low horizontal speed so the bot does not drift.
        bot->GetMotionMaster()->MoveJump(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                                         0.1f, 7.0f);
    }
    else if (roll < uint32(q.hopChance) + q.spinChance)
    {
        // A twirl, deliberately sloppy. Nobody lands a clean 360: you spin one
        // to three times, overshoot or come up short, and end facing somewhere
        // you did not start. A perfect 2pi turn reads as scripted.
        float turns = float(urand(1, 3));
        float sloppy = frand(-1.1f, 1.1f);                 // over/undershoot
        float delta = (turns * 2.0f * float(M_PI)) + sloppy;
        if (urand(0, 1))
            delta = -delta;                                // some people spin the other way
        bot->SetFacingTo(Position::NormalizeOrientation(bot->GetOrientation() + delta));
    }
    else if (roll < uint32(q.hopChance) + q.spinChance + q.glanceChance)
    {
        // Small glance, as if looking around.
        bot->SetFacingTo(Position::NormalizeOrientation(bot->GetOrientation() + frand(-1.6f, 1.6f)));
    }
    else if (roll < uint32(q.hopChance) + q.spinChance + q.glanceChance + q.emoteChance)
    {
        constexpr size_t kEmoteCount = sizeof(kIdleEmotes) / sizeof(kIdleEmotes[0]);
        bot->HandleEmoteCommand(kIdleEmotes[urand(0, kEmoteCount - 1)]);
    }
    else if (bot->getStandState() == UNIT_STAND_STATE_SIT && urand(0, 2) == 0)
    {
        // Get back up.
        //
        // Sitting was a one-way latch: the core only clears it on movement or
        // damage, and about 90% of bots are stationary at any moment, so seated
        // bots accumulated. Measured after five minutes: 158 seated against 342
        // standing, and still climbing, which ends with a world of statues that
        // happen to be sitting down. Standing back up makes it a rhythm rather
        // than a ratchet.
        bot->SetStandState(UNIT_STAND_STATE_STAND);
    }
    else if (urand(0, 5) == 0 && bot->getStandState() == UNIT_STAND_STATE_STAND && !bot->IsMounted())
    {
        // Sit down. People park themselves constantly -- at inns, on the steps
        // of the bank, next to a flight master -- and a crowd of figures all
        // standing at attention is one of the things that makes a server read as
        // artificial.
        //
        // Measured motivation: across 500 bots only about 10% are moving at any
        // moment (40 to 55 in a census), so most of the world is standing still
        // regardless. Standing still while seated at least looks deliberate.
        //
        // Cosmetic only: the core clears the sit state as soon as the bot moves
        // or is attacked, so this cannot strand anybody. Skipped while mounted,
        // since sitting on a mount is not a thing.
        //
        // The 1-in-6 matters. This branch is the fall-through of the quirk roll,
        // and the four profile weights sum to between 70 and 84, so without a
        // gate it would fire on a fifth to five sixths of every opportunity --
        // the "still, patient" profile would sit 84% of the time. Everyone seated
        // is as wrong as everyone at attention. The remainder goes back to doing
        // nothing, which is what it did before.
        bot->SetStandState(UNIT_STAND_STATE_SIT);
    }
}

// Evict per-bot state for bots that are no longer in world.
//
// Every one of these maps was insert-only. Bots churn constantly -- the level
// bracket system logs them out and back in, and PeriodicOnlineOffline rotates
// the roster -- so on a long-running server each map accumulated entries for
// bots that no longer exist. g_botDestinations was the worst offender, holding
// a vector of up to sixteen destinations per guid forever.
//
// Runs on the world thread against the live player list, so a single pass is
// enough; no per-entry ObjectAccessor lookups.
static void SweepStaleBotState()
{
    std::unordered_set<uint64_t> live;
    live.reserve(1024);
    for (auto const& itr : ObjectAccessor::GetPlayers())
        if (itr.second)
            live.insert(itr.second->GetGUID().GetRawValue());

    auto prune = [&live](auto& container)
    {
        for (auto it = container.begin(); it != container.end(); )
        {
            if (live.find(it->first) == live.end())
                it = container.erase(it);
            else
                ++it;
        }
    };

    {
        std::lock_guard<std::mutex> lock(g_botDestinationsMutex);
        prune(g_botDestinations);
        prune(g_botDestinationsMeta);
    }
    {
        std::lock_guard<std::mutex> lock(g_botPersonalityMutex);
        prune(g_botPersonalityCache);
        for (auto it = g_botPersonalityLoaded.begin(); it != g_botPersonalityLoaded.end(); )
            it = (live.find(*it) == live.end()) ? g_botPersonalityLoaded.erase(it) : std::next(it);
    }
    {
        std::lock_guard<std::mutex> lock(g_lastActionOutcomeMutex);
        prune(g_lastActionOutcome);
    }
    {
        std::lock_guard<std::mutex> lock(botPlayerMessagesMutex);
        prune(botPlayerMessages);
    }

    prune(ollamaBotStates);
    prune(ollamaBotPlans);
    prune(ollamaPrefetchedPlans);
    prune(g_nextQuirkTime);
    prune(g_nextSocialTime);

    for (auto it = ollamaTakenOver.begin(); it != ollamaTakenOver.end(); )
        it = (live.find(*it) == live.end()) ? ollamaTakenOver.erase(it) : std::next(it);
}

void OllamaBotControlLoop::OnUpdate(uint32 /*diff*/)
{
    if (!g_EnableOllamaBotControl) return;

    // Periodic eviction of state belonging to bots that have gone away.
    {
        static uint32 nextSweepMs = 0;
        uint32 nowMs = getMSTime();
        if (nowMs >= nextSweepMs)
        {
            nextSweepMs = nowMs + 60000;   // once a minute is ample for bot churn
            SweepStaleBotState();
        }
    }

    // Apply any LLM replies that arrived since the last tick. This runs on the
    // world thread, so re-resolving the guid here is the point at which it is
    // safe to touch the bot at all.
    {
        std::vector<OllamaPendingReply> ready;
        {
            std::lock_guard<std::mutex> lock(ollamaPendingRepliesMutex);
            ready.swap(ollamaPendingReplies);
        }

        for (auto const& pending : ready)
        {
            Player* bot = ObjectAccessor::FindPlayer(pending.botGuid);

            if (bot && bot->IsInWorld() && !pending.reply.empty())
            {
                if (g_EnableOllamaBotBuddyDebug)
                {
                    LOG_INFO("server.loading", "[OllamaBotBuddy] LLM reply for \'{}\':\n{}",
                             bot->GetName(), EscapeBracesForFmt(pending.reply));
                }

                std::string jsonOnly = ExtractFirstJsonObject(pending.reply);
                if (!jsonOnly.empty())
                {
                    // Store the plan and run its first step now. Later steps run
                    // on subsequent ticks without another LLM round trip.
                    try
                    {
                        auto root = nlohmann::json::parse(jsonOnly);

                        OllamaBotPlan plan;
                        plan.intent = root.value("intent", "");

                        if (root.contains("steps") && root["steps"].is_array())
                        {
                            for (auto const& step : root["steps"])
                                plan.steps.push_back(step.dump());
                        }
                        else if (root.contains("command"))
                        {
                            // Older single-command shape.
                            plan.steps.push_back(root["command"].dump());
                        }

                        if (!plan.steps.empty())
                        {
                            // If a plan is still running, this reply is the
                            // speculative prefetch for after it finishes. Park
                            // it rather than interrupting the bot mid-sequence.
                            auto activeIt = ollamaBotPlans.find(pending.stateKey);
                            if (activeIt != ollamaBotPlans.end() && activeIt->second.next < activeIt->second.steps.size())
                            {
                                ollamaPrefetchedPlans[pending.stateKey] = std::move(plan);

                                auto stIt = ollamaBotStates.find(pending.stateKey);
                                if (stIt != ollamaBotStates.end())
                                    stIt->second.busy = false;
                                continue;
                            }

                            std::string say = root.value("say", "");
                            std::string reasoning = root.value("reasoning", "");

                            // Execute step 0 immediately so the bot reacts at the
                            // same latency it did before plans existed.
                            nlohmann::json first;
                            first["command"] = nlohmann::json::parse(plan.steps[0]);
                            if (!say.empty())       first["say"] = say;
                            if (!reasoning.empty()) first["reasoning"] = reasoning;

                            bool ok = ParseAndExecuteBotJson(bot, first.dump());
                            plan.next = 1;

                            // A failed opening step means the plan was built on a
                            // bad read of the world; do not run the rest of it.
                            if (ok && plan.next < plan.steps.size())
                                ollamaBotPlans[pending.stateKey] = std::move(plan);
                            else
                                ollamaBotPlans.erase(pending.stateKey);
                        }
                    }
                    catch (std::exception const& e)
                    {
                        LOG_ERROR("server.loading", "[OllamaBotBuddy] Plan parse error: {}", e.what());
                        ollamaBotPlans.erase(pending.stateKey);
                    }

                    // Rebuild the prompt so the latest command shows in history
                    std::string updatedPrompt = BuildBotPrompt(bot);
                    SendBuddyBotStateToPlayer(bot, bot, updatedPrompt);
                }
                else
                {
                    LOG_ERROR("server.loading",
                              "[OllamaBotBuddy] No valid JSON object found in LLM reply: {}", pending.reply);
                }
            }

            // Always clear busy, even when the bot vanished, or that guid would
            // never be queried again.
            auto it = ollamaBotStates.find(pending.stateKey);
            if (it != ollamaBotStates.end())
                it->second.busy = false;
        }
    }

    // Idle gate: with nobody logged in there is no one to watch the bot, but the
    // loop still issued an LLM request every few seconds. That pinned the model
    // in memory (4.3GB on this host) and burned GPU continuously for no benefit.
    //
    // Bots keep playing normally via playerbots AI while this is skipped; only
    // the LLM layer pauses, and it resumes the moment a real player logs in.
    {
        bool anyRealPlayer = false;
        for (auto const& itr : ObjectAccessor::GetPlayers())
        {
            Player* p = itr.second;
            if (!p || !p->IsInWorld()) continue;
            // WorldSession::IsBot() is set at session creation, so it is correct
            // even during the window where a bot has logged in but its
            // PlayerbotAI is not yet attached. Testing GetPlayerbotAI() alone
            // briefly misidentified logging-in bots as real players, which was
            // enough to wake the LLM and pin the model in memory.
            if (p->GetSession() && !p->GetSession()->IsBot()) { anyRealPlayer = true; break; }
        }
        if (!anyRealPlayer && g_OllamaRequirePlayerOnline) return;
    }

    static std::unordered_set<uint64_t> testAdoptedBots;   // stable across ticks

    for (auto const& itr : ObjectAccessor::GetPlayers())
    {
        Player* bot = itr.second;
        if (!bot->IsInWorld()) continue;

        // Idle fidgeting, applied to every bot in the world regardless of
        // whether the LLM drives it. Cheap: a timestamp compare for most bots
        // on most ticks.
        ApplyHumanMovement(bot);

        // Nothing in the command vocabulary works while dead: there is no
        // release, no corpse run, no revive. Hand the bot back to playerbots,
        // which has a dead-state strategy for exactly this, and stop paying for
        // inference that can only produce impossible commands.
        if (!bot->IsAlive())
        {
            ReleaseBotToPlayerbots(bot);
            continue;
        }

        std::string botName = bot->GetName();

        // Which bots the LLM drives.
        //
        // This was a hardcoded name check ("Ollamatest"), marked by the author as
        // a temporary testing marker. It made group content impossible: a dungeon
        // needs a party of AI-driven bots, not one.
        //
        // ControlPartyBots is the useful default for dungeons and raids: the LLM
        // drives exactly the bots grouped with a real player, while the other
        // hundreds of world bots keep using normal (cheap) playerbots AI. That
        // keeps LLM cost proportional to the party you are actually playing with.
        {
            bool selected = false;

            if (!g_OllamaBotNames.empty())
            {
                std::stringstream namesStream(g_OllamaBotNames);
                std::string entry;
                while (std::getline(namesStream, entry, ','))
                {
                    entry.erase(0, entry.find_first_not_of(" \t"));
                    entry.erase(entry.find_last_not_of(" \t") + 1);
                    if (!entry.empty() && entry == botName) { selected = true; break; }
                }
            }

            // Testing hook: adopt arbitrary bots so the loop can be exercised
            // without a player online and without depending on a specific bot
            // still being in the world.
            if (!selected && g_OllamaTestBotCount > 0)
            {
                uint64_t tk = bot->GetGUID().GetRawValue();

                // Adopting whichever bots come first samples whatever most of
                // the 500 are doing, which is standing in a town: only 4% of
                // prompts captured that way had the bot in combat, so combat
                // decisions were barely sampled at all.
                // "combat" narrows the sample to bots that are
                // fighting, and hands the slot back when they stop, so the
                // sample keeps following live combat instead of freezing on
                // whoever happened to be fighting first.
                bool const wantCombat = (g_OllamaTestBotFilter == "combat");
                bool const eligible   = !wantCombat || bot->IsInCombat();

                if (testAdoptedBots.count(tk))
                {
                    if (wantCombat && !eligible)
                        testAdoptedBots.erase(tk);
                    else
                        selected = true;
                }
                else if (eligible && testAdoptedBots.size() < g_OllamaTestBotCount)
                {
                    testAdoptedBots.insert(tk);
                    selected = true;
                }
            }

            if (!selected && g_OllamaControlParty)
            {
                if (Group* grp = bot->GetGroup())
                {
                    for (GroupReference* ref = grp->GetFirstMember(); ref; ref = ref->next())
                    {
                        Player* member = ref->GetSource();
                        // A real player is one with no playerbot AI attached.
                        if (member && !PlayerbotsMgr::instance().GetPlayerbotAI(member))
                        {
                            selected = true;
                            break;
                        }
                    }
                }
            }

            if (!selected)
            {
                // No longer eligible (left the party, or names list changed).
                ReleaseBotToPlayerbots(bot);
                continue;
            }
        }

        // Do not take over bots inside battlegrounds, arenas or LFG dungeons.
        //
        // The takeover below clears every playerbots strategy. Inside a
        // battleground that destroys the whole BG tactics layer ("bg tactics",
        // "bg role", "bg select objective", "protect fc", "attack enemy flag
        // carrier"), which is purpose-built, well tested, and far better at
        // capturing flags and holding objectives than a generic LLM issuing one
        // movement command every few seconds.
        //
        // The same applies to instanced PvE: clearing strategies also removes
        // "tank"/"tank assist"/"dps assist"/"cc"/"aoe" and the threat handling
        // that keeps a dungeon group alive.
        if (bot->InBattleground() || bot->InArena() || bot->inRandomLfgDungeon() ||
            bot->InBattlegroundQueue())
        {
            // Give the bot its native AI back on the way in, otherwise it fights
            // the battleground with whatever we left it holding.
            ReleaseBotToPlayerbots(bot);
            continue;
        }

        // Take over the non-combat brain only.
        //
        // BOT_STATE_COMBAT is deliberately left alone. The attack handler ends
        // with ChangeEngine(BOT_STATE_COMBAT), handing the fight to playerbots'
        // rotation -- but clearing that state first left it with no strategies
        // to run, so an LLM-driven bot auto-attacked with a full spellbook and
        // 100 rage. Measured: the model chose plain "attack" 8/8 in a fight
        // where Shield Slam was named as its strongest option, because issuing
        // abilities was never its job.
        //
        // Same division as battlegrounds: the LLM decides what to do and where
        // to go, playerbots decides how to fight.
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (ai)
        {
            // Only the non-combat brain. BOT_STATE_COMBAT holds the rotation and
            // BOT_STATE_DEAD holds the "dead" strategy that releases and runs
            // back to the corpse. Clearing either left the bot with no way to
            // fight or no way to resurrect, while the LLM kept issuing commands
            // that a corpse or a strategy-less engine cannot carry out.
            // Gated so the contribution of the deterministic non-combat AI can
            // be measured rather than argued about. Default keeps the clear.
            if (g_OllamaClearNonCombat)
            {
                ai->ClearStrategies(BOT_STATE_NON_COMBAT);

                // ... but put "default" back. Despite the name, that strategy
                // is mod-playerbots' WorldPacketHandlerStrategy
                // (StrategyContext.h: creators["default"] = world_packet), and
                // it is not autonomous behaviour at all -- it is the bot's
                // entire reply layer for incoming packets:
                //
                //   group invite   -> accept invitation
                //   trade status   -> accept trade
                //   loot response  -> store loot
                //   use game object-> add loot
                //   item push      -> unlock/open/equip
                //   very often     -> loot roll
                //   release spirit -> release, revive from corpse
                //   activate taxi  -> taxi, guild invite, lfg proposal
                //
                // Wiping it left party bots unable to accept an invite or a
                // trade, unable to store or roll on loot, and unable to
                // release after dying. It survived in the combat and dead
                // engines, so this only bit out of combat -- which is exactly
                // when looting, trading and inviting happen. Reported from
                // several hours of live play.
                //
                // It carries no travel or questing behaviour, which is what
                // the clear is actually for, so restoring it does not put the
                // deterministic brain back in competition with the LLM.
                ai->ChangeStrategy("+default", BOT_STATE_NON_COMBAT);
            }
            ollamaTakenOver.insert(bot->GetGUID().GetRawValue());

            // One-shot proof that the combat and dead brains actually survive
            // the takeover. Both were being wiped before, leaving bots to
            // auto-attack with a full spellbook and to stay dead permanently.
            // Logged once per bot so a real session shows what it really has.
            if (g_EnableOllamaBotBuddyDebug)
            {
                static std::unordered_set<uint64_t> logged;
                uint64_t lk = bot->GetGUID().GetRawValue();
                if (!logged.count(lk))
                {
                    logged.insert(lk);
                    auto join = [](std::vector<std::string> const& v)
                    {
                        std::string out;
                        for (auto const& x : v) { if (!out.empty()) out += ","; out += x; }
                        return out.empty() ? std::string("(none)") : out;
                    };
                    LOG_INFO("server.loading",
                             "[OllamaBotBuddy] takeover '{}' combat=[{}] dead=[{}] noncombat=[{}]",
                             bot->GetName(),
                             join(ai->GetStrategies(BOT_STATE_COMBAT)),
                             join(ai->GetStrategies(BOT_STATE_DEAD)),
                             join(ai->GetStrategies(BOT_STATE_NON_COMBAT)));
                }
            }
        } else {
            continue;
        }

        uint64_t guid = bot->GetGUID().GetRawValue();
        OllamaBotState& state = ollamaBotStates[guid];

        // Only process if not already waiting for LLM, and not more often than
        // the configured interval. lastRequest was previously recorded but never
        // checked, so bots re-queried as fast as Ollama could answer.
        time_t now = time(nullptr);

        // If this bot has an unfinished plan, run its next step instead of
        // asking the LLM again. This is what makes behaviour multi-step: the
        // model states an intent once and the bot follows through, and it costs
        // one inference per plan rather than one per action.
        {
            auto planIt = ollamaBotPlans.find(guid);
            if (planIt != ollamaBotPlans.end())
            {
                OllamaBotPlan& plan = planIt->second;

                if (plan.next >= plan.steps.size())
                {
                    ollamaBotPlans.erase(planIt);

                    // Adopt the speculatively fetched plan so the bot continues
                    // without waiting on a fresh round trip.
                    auto pre = ollamaPrefetchedPlans.find(guid);
                    if (pre != ollamaPrefetchedPlans.end())
                    {
                        ollamaBotPlans[guid] = std::move(pre->second);
                        ollamaPrefetchedPlans.erase(pre);
                    }
                }
                else if ((now - state.lastRequest) >= static_cast<time_t>(g_OllamaDecisionInterval))
                {
                    nlohmann::json wrapped;
                    try
                    {
                        wrapped["command"] = nlohmann::json::parse(plan.steps[plan.next]);
                        if (!plan.intent.empty())
                            wrapped["reasoning"] = "Continuing plan: " + plan.intent;
                    }
                    catch (std::exception const&)
                    {
                        ollamaBotPlans.erase(planIt);
                        continue;
                    }

                    bool const isFinalStep = (plan.next + 1 >= plan.steps.size());

                    ++plan.next;
                    state.lastRequest = now;

                    // Execute BEFORE prefetching. FireLlmRequest calls
                    // BuildBotPrompt synchronously on this thread, and that
                    // rebuilds the cached destination list whenever the bot has
                    // moved more than 15 yards or the 10s TTL has lapsed -- both
                    // near-certain by the last step of a plan. Prefetching first
                    // therefore resolved this step's destination_index against a
                    // list the model never saw, and the range check still passed,
                    // so the bot silently walked to the wrong place.
                    //
                    // Measured over 8 consecutive prompt pairs captured from the
                    // live server: 7 of 8 rebuilds remapped indices, three of them
                    // remapping every single entry. In one, index [0] changed from
                    // "Balir Frosthammer (5y)" (the quest giver the plan was
                    // heading to) into "Coldridge Mountaineer (3y)".
                    bool const stepOk = ParseAndExecuteBotJson(bot, wrapped.dump());

                    // Now it is safe to rebuild the destination list. The step has
                    // already resolved, and the inference still overlaps the actual
                    // movement, which takes seconds -- so the prefetch keeps its
                    // benefit. Skipping the request when the step failed also saves
                    // a slot of very scarce capacity (~0.2 decisions/second): the
                    // plan is discarded just below, and the old code fired the
                    // request only to erase the result.
                    if (stepOk && isFinalStep && !state.busy &&
                        ollamaPrefetchedPlans.find(guid) == ollamaPrefetchedPlans.end())
                        FireLlmRequest(bot, guid, state);

                    // A failed step invalidates the rest: the world has moved on
                    // from whatever the plan assumed, so re-plan next tick. The
                    // prefetch assumed this step succeeded, so it goes too.
                    if (!stepOk)
                    {
                        ollamaBotPlans.erase(guid);
                        ollamaPrefetchedPlans.erase(guid);
                    }
                    else if (plan.next >= plan.steps.size())
                    {
                        ollamaBotPlans.erase(guid);

                        auto pre = ollamaPrefetchedPlans.find(guid);
                        if (pre != ollamaPrefetchedPlans.end())
                        {
                            ollamaBotPlans[guid] = std::move(pre->second);
                            ollamaPrefetchedPlans.erase(pre);
                        }
                    }

                    continue;   // no LLM request this tick
                }
                else
                {
                    continue;   // plan pending, waiting on the interval
                }
            }
        }

        // Deterministic first.
        //
        // BuildSituationAssessment has already worked out the right command for
        // the common cases. Serialising that into a 24,000 character prompt and
        // waiting ~21s for a 7B model to agree bought nothing: compliance was 0%
        // until the text was moved to the end of the prompt, and 36% of decisions
        // still came back as an action the executor refused.
        //
        // Doing it here skips inference entirely on those ticks. Anything the
        // assessment cannot decide still goes to the model, which is where speech
        // and genuinely open-ended choices live.
        if (g_OllamaDeterministicActions && !state.busy &&
            (now - state.lastRequest) >= static_cast<time_t>(g_OllamaDecisionInterval))
        {
            nlohmann::json decided;
            BuildSituationAssessment(bot, &decided);

            if (!decided.is_null())
            {
                nlohmann::json wrapped;
                wrapped["command"]   = decided;
                wrapped["reasoning"] = "situation assessment";

                state.lastRequest = time(nullptr);
                ParseAndExecuteBotJson(bot, wrapped.dump());
                continue;   // no inference this tick
            }
        }

        if (!state.busy && (now - state.lastRequest) >= static_cast<time_t>(g_OllamaDecisionInterval))
        {
            state.busy = true;
            state.lastRequest = time(nullptr);

            std::string prompt = BuildBotPrompt(bot);
            // Built here, on the world thread, because it reads the bot's map.
            size_t destCount = 0;
            {
                std::lock_guard<std::mutex> lock(g_botDestinationsMutex);
                auto it = g_botDestinations.find(bot->GetGUID().GetRawValue());
                if (it != g_botDestinations.end()) destCount = it->second.size();
            }
            nlohmann::json actionSchema = BuildBotActionSchema(bot, destCount);

            if (g_EnableOllamaBotBuddyDebug)
            {
                //LOG_INFO("server.loading", "[OllamaBotBuddy] Sending prompt for bot '{}': {}", botName, prompt);
            }

            // The HTTP call blocks for seconds, so it runs off the world thread.
            // It must not touch the bot: capturing a raw Player* and using it
            // after the call is a use-after-free if the bot goes away during
            // inference. Park the reply instead; OnUpdate applies it.
            ObjectGuid botGuid = bot->GetGUID();
            std::thread([botGuid, guid, prompt, actionSchema]() {
                std::string llmReply = QueryOllamaLLM(prompt, actionSchema);

                std::lock_guard<std::mutex> lock(ollamaPendingRepliesMutex);
                ollamaPendingReplies.push_back({botGuid, guid, std::move(llmReply)});
            }).detach();
        }
    }
}
