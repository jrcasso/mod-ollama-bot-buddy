#include "mod-ollama-bot-buddy_api.h"
#include "mod-ollama-bot-buddy_config.h"
#include "mod-ollama-bot-buddy_loop.h"
#include "Playerbots.h"
#include "LootObjectStack.h"
#include "PlayerbotAI.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Chat.h"
#include "Log.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Cell.h"
#include "Map.h"
#include "Event.h"
#include "QuestDef.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "GossipDef.h"
#include <sstream>

// Constants for interaction and combat ranges
#define INTERACTION_DISTANCE 5.5f
#define ATTACK_DISTANCE 5.0f

namespace BotBuddyAI
{
    bool MoveTo(Player* bot, float x, float y, float z)
    {
        if (!bot) return false;
        
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!ai) return false;
        
        if (g_EnableOllamaBotBuddyDebug) {
            LOG_INFO("server.loading", "[OllamaBotBuddy] MoveTo called for bot {} to position ({:.2f}, {:.2f}, {:.2f})", 
                bot->GetName(), x, y, z);
        }
        
        // Validate coordinates are reasonable 
        if (std::isnan(x) || std::isnan(y) || std::isnan(z) || 
            std::isinf(x) || std::isinf(y) || std::isinf(z)) {
            if (g_EnableOllamaBotBuddyDebug) {
                LOG_ERROR("server.loading", "[OllamaBotBuddy] Invalid coordinates for MoveTo: ({}, {}, {})", x, y, z);
            }
            return false;
        }
        
        // Clear existing movement
        bot->GetMotionMaster()->Clear(false);
        bot->StopMoving();
        
        // Use direct movement for immediate response
        bot->GetMotionMaster()->MovePoint(0, x, y, z);
        
        // Also try using the bot's AI movement system as backup
        std::ostringstream coords;
        coords << x << ";" << y << ";" << z;
        Event event = Event("", coords.str());
        ai->DoSpecificAction("go", event, /*silent=*/true);
        
        if (g_EnableOllamaBotBuddyDebug) {
            float distance = sqrt(pow(x - bot->GetPositionX(), 2) + 
                                pow(y - bot->GetPositionY(), 2) + 
                                pow(z - bot->GetPositionZ(), 2));
            LOG_INFO("server.loading", "[OllamaBotBuddy] MoveTo initiated, distance: {:.2f}", distance);
        }
        
        return true;
    }

    bool Attack(Player* bot, ObjectGuid guid)
    {
        if (!bot || !guid) return false;

        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!ai) return false;

        Unit* target = ObjectAccessor::GetUnit(*bot, guid);
        if (!target || !bot->IsWithinLOSInMap(target)) {
            if (g_EnableOllamaBotBuddyDebug) {
                LOG_INFO("server.loading", "[OllamaBotBuddy] Target not found or not in LOS for guid: {}", guid.GetCounter());
            }
            return false;
        }

        // CRITICAL: Validate target before attacking to prevent friendly fire
        if (!bot->IsValidAttackTarget(target)) {
            if (g_EnableOllamaBotBuddyDebug) {
                LOG_INFO("server.loading", "[OllamaBotBuddy] Invalid attack target: {} - not attackable", target->GetName());
            }
            return false;
        }

        // Check if target is friendly - absolutely prevent attacking friendlies
        if (bot->IsFriendlyTo(target)) {
            if (g_EnableOllamaBotBuddyDebug) {
                LOG_INFO("server.loading", "[OllamaBotBuddy] Refusing to attack friendly target: {}", target->GetName());
            }
            return false;
        }

        // Additional safety: check if target is in same group or guild
        if (Player* targetPlayer = target->ToPlayer()) {
            if (bot->IsInSameGroupWith(targetPlayer) || (bot->GetGuildId() != 0 && bot->GetGuildId() == targetPlayer->GetGuildId())) {
                if (g_EnableOllamaBotBuddyDebug) {
                    LOG_INFO("server.loading", "[OllamaBotBuddy] Refusing to attack group/guild member: {}", target->GetName());
                }
                return false;
            }
        }

        // Check if target is dead - if so, refuse to attack and suggest looting instead
        if (!target->IsAlive()) {
            if (g_EnableOllamaBotBuddyDebug) {
                LOG_INFO("server.loading", "[OllamaBotBuddy] REFUSING to attack dead target: {} - it should be looted, not attacked", target->GetName());
            }
            return false; // Explicitly refuse to attack dead creatures
        }
        
        // Check if target is GM
        if (target->ToPlayer() && target->ToPlayer()->IsGameMaster()) {
            if (g_EnableOllamaBotBuddyDebug) {
                LOG_INFO("server.loading", "[OllamaBotBuddy] Target is GM: {}", target->GetName());
            }
            return false;
        }

        if (g_EnableOllamaBotBuddyDebug)
        {
            LOG_INFO("server.loading", "[OllamaBotBuddy] Bot {} attacking target {} (guid: {})", 
                bot->GetName(), target->GetName(), guid.GetCounter());
        }

        // Calculate ranges properly using AzerothCore standards
        float currentDistance = bot->GetExactDist2d(target);
        float meleeRange = bot->GetMeleeRange(target); // This includes combat reach calculation
        float combatReach = bot->GetCombatReach() + target->GetCombatReach();
        
        if (g_EnableOllamaBotBuddyDebug) {
            LOG_INFO("server.loading", "[OllamaBotBuddy] Combat distances - Current: {:.2f}, Melee: {:.2f}, CombatReach: {:.2f}", 
                currentDistance, meleeRange, combatReach);
        }

        // Set target in AI context immediately for proper behavior
        ai->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(target);
        ai->GetAiObjectContext()->GetValue<ObjectGuid>("pull target")->Set(guid);
        
        // Set selection and target properly
        bot->SetSelection(guid);
        bot->SetTarget(guid);
        
        // Face the target if in combat or close
        if (bot->IsInCombat() || currentDistance <= meleeRange + 5.0f) {
            bot->SetFacingToObject(target);
        }
        
        // Check if we need to move closer for melee combat
        if (currentDistance > meleeRange) {
            if (g_EnableOllamaBotBuddyDebug) {
                LOG_INFO("server.loading", "[OllamaBotBuddy] Moving to melee range - distance {:.2f} > meleeRange {:.2f}", 
                    currentDistance, meleeRange);
            }
            
            // Clear any existing movement and start chasing
            bot->GetMotionMaster()->Clear();
            
            // Use MoveChase with proper melee range - this should get the bot close enough to attack
            bot->GetMotionMaster()->MoveChase(target, 0.0f); // 0.0f means use default melee range
            
            // Change to combat engine to enable combat actions
            ai->ChangeEngine(BOT_STATE_COMBAT);
            
            // Also use playerbot AI movement action as backup
            Event moveEvent = Event("", "");
            ai->DoSpecificAction("reach melee", moveEvent, /*silent=*/true);
            
            return true; // Movement initiated, attack will happen when in range
        }
        
        // We're in melee range - initiate combat
        if (g_EnableOllamaBotBuddyDebug) {
            LOG_INFO("server.loading", "[OllamaBotBuddy] In melee range, starting combat");
        }
        
        // Change to combat engine to enable combat actions
        ai->ChangeEngine(BOT_STATE_COMBAT);
        
        // Face the target before attacking
        bot->SetFacingToObject(target);
        
        // Start auto-attack
        bot->Attack(target, true);
        
        // Use playerbot AI actions for better combat behavior
        Event event = Event("", "");
        bool result = false;
        
        // Try playerbot combat actions in order of preference
        if (ai->IsTank(bot)) {
            result = ai->DoSpecificAction("tank assist", event, /*silent=*/true);
        } else {
            result = ai->DoSpecificAction("dps assist", event, /*silent=*/true);
        }
        
        // Fallback to melee action if assist actions fail
        if (!result) {
            result = ai->DoSpecificAction("melee", event, /*silent=*/true);
        }
        
        if (g_EnableOllamaBotBuddyDebug) {
            LOG_INFO("server.loading", "[OllamaBotBuddy] Combat initiated, playerbot action result: {}", 
                result ? "SUCCESS" : "FALLBACK_TO_MANUAL");
        }
        
        return true;
    }

    bool Interact(Player* bot, ObjectGuid guid)
    {
        if (!bot || !guid) return false;

        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!ai) return false;

        if (Creature* creature = ObjectAccessor::GetCreature(*bot, guid))
        {
            // Check interaction distance FIRST - move closer if needed
            float distance = bot->GetDistance(creature);
            // Refuse targets the bot cannot reach inside a decision interval.
            // Returning false hands this tick back to the native brain, which
            // ClearNonCombatStrategies=0 leaves running -- strictly better than
            // burning the interval walking toward something 2000 yards away.
            if (distance > g_OllamaBotMaxTargetDistance)
            {
                if (g_EnableOllamaBotBuddyDebug)
                    LOG_INFO("server.loading", "[OllamaBotBuddy] Bot {} REJECTED unreachable target {} at {:.1f}y (max {:.1f})",
                        bot->GetName(), creature->GetName(), distance, g_OllamaBotMaxTargetDistance);
                return false;
            }
            if (distance > INTERACTION_DISTANCE)
            {
                // Too far - move closer first
                if (g_EnableOllamaBotBuddyDebug) {
                    LOG_INFO("server.loading", "[OllamaBotBuddy] Bot {} moving to interact with {} at distance {:.1f}", 
                        bot->GetName(), creature->GetName(), distance);
                }
                
                // Calculate a position close to the creature but not directly on top
                float angle = creature->GetAngle(bot);
                float destX = creature->GetPositionX() + cos(angle + M_PI) * 3.0f; // 3 yards away
                float destY = creature->GetPositionY() + sin(angle + M_PI) * 3.0f;
                float destZ = creature->GetPositionZ();
                
                bot->GetMotionMaster()->Clear();
                bot->GetMotionMaster()->MovePoint(0, destX, destY, destZ);
                return true; // Movement initiated, interaction will happen next cycle
            }
            
            // Check if this is a quest giver and handle quest interaction properly
            if (creature->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_QUESTGIVER))
            {
                return InteractWithQuestGiver(bot, creature);
            }
            else
            {
                // For non-quest NPCs, use gossip hello action
                bot->SetFacingToObject(creature);
                Event event = Event("", std::to_string(guid.GetCounter()));
                return ai->DoSpecificAction("gossip hello", event, /*silent=*/true);
            }
        }
        else if (GameObject* go = ObjectAccessor::GetGameObject(*bot, guid))
        {
            // Check interaction distance FIRST - move closer if needed  
            float distance = bot->GetDistance(go);
            float interactionDist = go->GetInteractionDistance();
            if (distance > g_OllamaBotMaxTargetDistance)
            {
                if (g_EnableOllamaBotBuddyDebug)
                    LOG_INFO("server.loading", "[OllamaBotBuddy] Bot {} REJECTED unreachable object at {:.1f}y (max {:.1f})",
                        bot->GetName(), distance, g_OllamaBotMaxTargetDistance);
                return false;
            }
            if (distance > interactionDist)
            {
                // Too far - move closer first
                if (g_EnableOllamaBotBuddyDebug) {
                    LOG_INFO("server.loading", "[OllamaBotBuddy] Bot {} moving to interact with {} at distance {:.1f}", 
                        bot->GetName(), go->GetName(), distance);
                }
                
                // Calculate a position close to the object but not directly on top
                float angle = go->GetAngle(bot);
                float destX = go->GetPositionX() + cos(angle + M_PI) * 2.0f; // 2 yards away
                float destY = go->GetPositionY() + sin(angle + M_PI) * 2.0f;
                float destZ = go->GetPositionZ();
                
                bot->GetMotionMaster()->Clear();
                bot->GetMotionMaster()->MovePoint(0, destX, destY, destZ);
                return true; // Movement initiated, interaction will happen next cycle
            }
            
            // Check if this is a quest giver game object
            if (go->GetGoType() == GAMEOBJECT_TYPE_QUESTGIVER)
            {
                return InteractWithQuestGiver(bot, go);
            }
            else
            {
                // Use the bot's AI system to handle interaction with game objects
                bot->SetFacingToObject(go);
                Event event = Event("", go->GetGOInfo()->name);
                return ai->DoSpecificAction("use", event, /*silent=*/true);
            }
        }
        return false;
    }

    bool InteractWithQuestGiver(Player* bot, WorldObject* questGiver)
    {
        if (!bot || !questGiver) return false;

        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!ai) return false;

        // Check interaction distance
        if (bot->GetDistance(questGiver) > INTERACTION_DISTANCE)
        {
            return false;
        }

        // Face the quest giver
        if (!bot->HasInArc(CAST_ANGLE_IN_FRONT, questGiver, sPlayerbotAIConfig.sightDistance))
            bot->SetFacingToObject(questGiver);

        ObjectGuid guid = questGiver->GetGUID();
        
        // Prepare the quest menu for this quest giver
        bot->PrepareQuestMenu(guid);
        QuestMenu& questMenu = bot->PlayerTalkClass->GetQuestMenu();

        bool foundQuestAction = false;

        // Process all available quest menu items
        for (uint32 i = 0; i < questMenu.GetMenuItemCount(); ++i)
        {
            QuestMenuItem const& menuItem = questMenu.GetItem(i);
            Quest const* quest = sObjectMgr->GetQuestTemplate(menuItem.QuestId);
            if (!quest) continue;

            QuestStatus status = bot->GetQuestStatus(menuItem.QuestId);
            
            // Handle completed quests first (highest priority)
            if (status == QUEST_STATUS_COMPLETE && bot->CanRewardQuest(quest, false))
            {
                // Turn in the quest using the playerbot action system
                TurnInQuest(bot, menuItem.QuestId);
                foundQuestAction = true;
                
                if (g_EnableOllamaBotBuddyDebug)
                {
                    LOG_INFO("server.loading", "[OllamaBotBuddy] Bot {} turning in quest {}: {}", 
                        bot->GetName(), menuItem.QuestId, quest->GetTitle());
                }
            }
            // Handle new quests that can be accepted
            else if (status == QUEST_STATUS_NONE && bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false))
            {
                // Only claim the turn if the quest was really accepted. Setting
                // this unconditionally is what made a permanently failing accept
                // look like success and left bots looping on the same giver.
                if (AcceptQuest(bot, menuItem.QuestId))
                    foundQuestAction = true;
                
                if (g_EnableOllamaBotBuddyDebug)
                {
                    LOG_INFO("server.loading", "[OllamaBotBuddy] Bot {} accepting quest {}: {}", 
                        bot->GetName(), menuItem.QuestId, quest->GetTitle());
                }
            }
        }

        // If we found quest actions, return success
        if (foundQuestAction)
        {
            return true;
        }

        // If no direct quest actions were available, try automatic gossip navigation
        if (Creature* creature = questGiver->ToCreature())
        {
            // Try to automatically navigate gossip menus for quest options
            if (AutoNavigateGossipForQuests(bot, creature))
            {
                return true;
            }
            
            // Fallback to basic gossip hello action
            Event event = Event("", std::to_string(guid.GetCounter()));
            return ai->DoSpecificAction("gossip hello", event, /*silent=*/true);
        }
        else if (GameObject* go = questGiver->ToGameObject())
        {
            // Use game object interaction
            Event event = Event("", go->GetGOInfo()->name);
            return ai->DoSpecificAction("use", event, /*silent=*/true);
        }

        return false;
    }

    bool AutoNavigateGossipForQuests(Player* bot, Creature* creature)
    {
        if (!bot || !creature) return false;

        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!ai) return false;

        // Start gossip interaction
        WorldPacket packet(CMSG_GOSSIP_HELLO);
        packet << creature->GetGUID();
        bot->GetSession()->HandleGossipHelloOpcode(packet);

        // Wait a moment for the server to process
        if (!bot->PlayerTalkClass) return false;

        GossipMenu& gossipMenu = bot->PlayerTalkClass->GetGossipMenu();
        QuestMenu& questMenu = bot->PlayerTalkClass->GetQuestMenu();

        // First priority: Handle direct quest menus
        for (uint32 i = 0; i < questMenu.GetMenuItemCount(); ++i)
        {
            QuestMenuItem const& menuItem = questMenu.GetItem(i);
            Quest const* quest = sObjectMgr->GetQuestTemplate(menuItem.QuestId);
            if (!quest) continue;

            QuestStatus status = bot->GetQuestStatus(menuItem.QuestId);
            
            if (status == QUEST_STATUS_COMPLETE && bot->CanRewardQuest(quest, false))
            {
                TurnInQuest(bot, menuItem.QuestId);
                return true;
            }
            else if (status == QUEST_STATUS_NONE && bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false))
            {
                // Same rule as above: report whether it was actually accepted.
                return AcceptQuest(bot, menuItem.QuestId);
            }
        }

        // Second priority: Navigate gossip menu for quest-related options
        GossipMenuItemContainer const& gossipItems = gossipMenu.GetMenuItems();
        for (auto const& item : gossipItems)
        {
            GossipMenuItem const* gossipItem = &item.second;
            std::string message = gossipItem->Message;
            
            // Look for quest-related gossip options with enhanced keyword matching
            if (message.find("quest") != std::string::npos || 
                message.find("Quest") != std::string::npos ||
                message.find("mission") != std::string::npos ||
                message.find("task") != std::string::npos ||
                message.find("reward") != std::string::npos ||
                message.find("complete") != std::string::npos ||
                message.find("turn in") != std::string::npos ||
                message.find("finish") != std::string::npos)
            {
                // Select this gossip option
                WorldPacket selectPacket(CMSG_GOSSIP_SELECT_OPTION);
                selectPacket << creature->GetGUID();
                selectPacket << gossipMenu.GetMenuId();
                selectPacket << item.first;
                selectPacket << std::string("");
                bot->GetSession()->HandleGossipSelectOptionOpcode(selectPacket);
                
                if (g_EnableOllamaBotBuddyDebug)
                {
                    LOG_INFO("server.loading", "[OllamaBotBuddy] Bot {} selected gossip option: {}", 
                        bot->GetName(), message);
                }
                return true;
            }
        }

        return false;
    }

    bool HasQuestsAvailable(Player* bot, WorldObject* questGiver)
    {
        if (!bot || !questGiver) return false;

        // For creatures, check their quest relations directly (more efficient)
        if (Creature* creature = questGiver->ToCreature())
        {
            // Check for completable quests first (highest priority)
            QuestRelationBounds qir = sObjectMgr->GetCreatureQuestInvolvedRelationBounds(creature->GetEntry());
            for (QuestRelations::const_iterator itr = qir.first; itr != qir.second; ++itr)
            {
                uint32 questId = itr->second;
                if (bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE && !bot->GetQuestRewardStatus(questId))
                {
                    return true; // Has quest ready to turn in
                }
            }
            
            // Check for available quests (secondary priority)
            QuestRelationBounds qr = sObjectMgr->GetCreatureQuestRelationBounds(creature->GetEntry());
            for (QuestRelations::const_iterator itr = qr.first; itr != qr.second; ++itr)
            {
                uint32 questId = itr->second;
                Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                if (quest && bot->GetQuestStatus(questId) == QUEST_STATUS_NONE && 
                    bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false))
                {
                    return true; // Has quest available to accept
                }
            }
        }
        // For game objects, check quest relations
        else if (GameObject* go = questGiver->ToGameObject())
        {
            // Check for completable quests
            QuestRelationBounds qir = sObjectMgr->GetGOQuestInvolvedRelationBounds(go->GetEntry());
            for (QuestRelations::const_iterator itr = qir.first; itr != qir.second; ++itr)
            {
                uint32 questId = itr->second;
                if (bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE && !bot->GetQuestRewardStatus(questId))
                {
                    return true;
                }
            }
            
            // Check for available quests
            QuestRelationBounds qr = sObjectMgr->GetGOQuestRelationBounds(go->GetEntry());
            for (QuestRelations::const_iterator itr = qr.first; itr != qr.second; ++itr)
            {
                uint32 questId = itr->second;
                Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                if (quest && bot->GetQuestStatus(questId) == QUEST_STATUS_NONE && 
                    bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false))
                {
                    return true;
                }
            }
        }
        
        return false;
    }

    bool CastSpell(Player* bot, uint32 spellId, Unit* target)
    {
        if (!bot) return false;
        
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!ai) return false;
        
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo) return false;

        // Passive spells, weapon and armour proficiencies are not castable, but the
        // model picks them out of its snapshot and issues them anyway -- observed
        // "Casting spell Polearms" and "Casting spell Simple Kilt", both with
        // spell range 0.0 (ITERATIONS row 93). Every such cast fails by
        // construction, so drop it and let the native brain use the tick.
        if (spellInfo->IsPassive())
        {
            if (g_EnableOllamaBotBuddyDebug)
                LOG_INFO("server.loading", "[OllamaBotBuddy] Bot {} REJECTED uncastable spell {}",
                    bot->GetName(), spellInfo->SpellName[0]);
            return false;
        }

        // Same unbounded-walk defect the interact path had (row 92): an
        // out-of-range spell falls through to "reach spell"/"reach melee" with no
        // cap, so a target 5797 yards away starts a cross-world walk that can
        // never complete. Measured: 93% of casts exceeded the spell's own range.
        if (target && bot->GetDistance(target) > g_OllamaBotMaxTargetDistance)
        {
            if (g_EnableOllamaBotBuddyDebug)
                LOG_INFO("server.loading", "[OllamaBotBuddy] Bot {} REJECTED unreachable cast target at {:.1f}y (max {:.1f})",
                    bot->GetName(), bot->GetDistance(target), g_OllamaBotMaxTargetDistance);
            return false;
        }
        
        // Set the target in the AI context if provided
        if (target) {
            ai->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(target);
            
            // Check range requirements for the spell
            float spellRange = spellInfo->GetMaxRange(false);
            float currentDistance = bot->GetDistance(target);
            bool isMeleeSpell = spellRange <= ATTACK_DISTANCE;
            
            if (g_EnableOllamaBotBuddyDebug) {
                LOG_INFO("server.loading", "[OllamaBotBuddy] Casting spell {} on target at distance {:.1f}, spell range: {:.1f}", 
                    spellInfo->SpellName[0], currentDistance, spellRange);
            }
            
            // Handle positioning for spell casting
            Event moveEvent = Event("", "");
            if (isMeleeSpell && !bot->IsWithinMeleeRange(target)) {
                // Need to get into melee range for melee spells
                ai->DoSpecificAction("reach melee", moveEvent, /*silent=*/true);
            } else if (!isMeleeSpell && currentDistance > spellRange) {
                // Need to get into spell range for ranged spells
                ai->DoSpecificAction("reach spell", moveEvent, /*silent=*/true);
            } else if (!isMeleeSpell && currentDistance < 5.0f && ai->IsRanged(bot)) {
                // Ranged character too close - back away for better positioning
                ai->DoSpecificAction("flee", moveEvent, /*silent=*/true);
            }
        }
        
        // Use the spell name directly as the action
        const char* spellName = spellInfo->SpellName[0];
        if (!spellName || !*spellName) return false;
        
        Event event = Event("", "");
        bool result = ai->DoSpecificAction(spellName, event, /*silent=*/true);
        
        // If spell casting by name fails, try using spell ID
        if (!result && target) {
            // Try alternative approaches
            std::string spellIdStr = std::to_string(spellId);
            event = Event("", spellIdStr);
            result = ai->DoSpecificAction("cast", event, /*silent=*/true);
        }
        
        if (g_EnableOllamaBotBuddyDebug) {
            LOG_INFO("server.loading", "[OllamaBotBuddy] Spell cast result for {}: {}", 
                spellName, result ? "SUCCESS" : "FAILED");
        }
        
        return result;
    }

    bool Say(Player* bot, const std::string& msg)
    {
        if (!bot || msg.empty()) return false;

        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!ai) return false;

        // This used to be:
        //
        //     Event event = Event("", msg);
        //     return ai->DoSpecificAction("say", event, /*silent=*/true);
        //
        // which threw the message away. SayAction::Execute is declared
        // `Execute(Event /*event*/)` -- the parameter is commented out and never
        // read. That action exists to emit canned playerbot chatter selected by a
        // qualifier ("low ammo" and friends) from sPlayerbotTextMgr; it has no
        // mechanism for arbitrary text. So every line the model generated was
        // discarded, silently, and the bot said either nothing or something
        // unrelated.
        //
        // That mattered more than any other instance of this bug class, because
        // speech is the one thing the LLM measurably does here that no rule could
        // (docs/PERSONAS.md): personality changes what a bot says and nothing
        // about what it does. The generated text was the whole justification for
        // the inference call, and it was being dropped on the floor.
        //
        // Rate limit, and only speak where someone could hear it.
        //
        // With the plumbing fixed the volume was the problem: 88 lines from 89
        // decisions, every bot narrating every action. Real players do not
        // announce that they are about to click a quest giver, and a party of
        // bots doing it in unison is worse than silence.
        //
        // This is deliberately code rather than a prompt instruction. Whether to
        // speak now is a cooldown and a proximity check, both of which the server
        // can answer exactly; asking a 7B model to be tastefully quiet is neither
        // reliable nor necessary.
        //
        // Called only from ParseAndExecuteBotJson, which runs on the world
        // thread, so a plain static map needs no lock.
        {
            static std::unordered_map<uint64_t, time_t> nextSay;
            uint64_t const key = bot->GetGUID().GetRawValue();
            time_t const now = time(nullptr);

            auto it = nextSay.find(key);
            if (it != nextSay.end() && now < it->second)
                return false;

            // Nobody in earshot means the line is wasted. Say carries about 30
            // yards, which is what mod-ollama-chat uses for the same decision.
            bool heard = false;
            for (auto const& itr : ObjectAccessor::GetPlayers())
            {
                Player* other = itr.second;
                if (!other || other == bot || !other->IsInWorld()) continue;
                if (bot->IsWithinDistInMap(other, 30.0f)) { heard = true; break; }
            }
            if (!heard)
                return false;

            // Jittered so a crowd of bots does not fall into lockstep.
            uint32 const base = g_OllamaSayCooldownSeconds;
            nextSay[key] = now + time_t(base + urand(0, base));
        }

        // PlayerbotAI::Say is the API for this, and it is what mod-ollama-chat
        // uses. It picks the faction language and calls Player::Say.
        bool const said = ai->Say(msg);

        if (g_EnableOllamaBotBuddyDebug)
        {
            LOG_INFO("server.loading", "[OllamaBotBuddy] {} says: {}",
                     bot->GetName(), EscapeBracesForFmt(msg));
        }

        return said;
    }

    bool FollowMaster(Player* bot)
    {
        if (!bot) return false;
        
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!ai) return false;
        
        // Use the bot's AI system to handle following
        Event event = Event("", "");
        return ai->DoSpecificAction("follow", event, /*silent=*/true);
    }

    bool StopMoving(Player* bot)
    {
        if (!bot) return false;
        
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!ai) return false;
        
        // Use the bot's AI system to handle stopping
        Event event = Event("", "");
        return ai->DoSpecificAction("stay", event, /*silent=*/true);
    }

    bool AcceptQuest(Player* bot, uint32 questId)
    {
        if (!bot || !bot->GetMap()) return false;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest) return false;

        // This used to delegate to the playerbot AI:
        //
        //     Event event = Event("", std::to_string(questId));
        //     return ai->DoSpecificAction("accept quest", event, /*silent=*/true);
        //
        // which can never work for a random bot. AcceptQuestAction::Execute
        // begins with
        //
        //     Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
        //     if (!requester) return false;
        //
        // The event built above has no owner and a random bot has no master, so
        // it returned false on its first line, every time. Callers then threw the
        // result away, so a quest that was never accepted looked accepted.
        //
        // Observed live: a level 26 draenei stood at Megelon for 30 minutes,
        // across 15 decisions, told each time that a quest giver was one yard
        // away, interacting each time, reporting no failure, and ending with
        // zero rows in character_queststatus.
        //
        // Accept it the way the core does for CMSG_QUESTGIVER_ACCEPT_QUEST:
        // find the giver in range, re-check, then AddQuestAndCheckCompletion.
        if (bot->GetQuestStatus(questId) != QUEST_STATUS_NONE) return false;
        if (!bot->CanTakeQuest(quest, false)) return false;
        if (!bot->CanAddQuest(quest, false)) return false;

        Object* giver = nullptr;
        for (auto const& pair : bot->GetMap()->GetCreatureBySpawnIdStore())
        {
            Creature* c = pair.second;
            if (!c || !c->hasQuest(questId)) continue;
            if (!bot->IsWithinDistInMap(c, INTERACTION_DISTANCE)) continue;
            giver = c;
            break;
        }
        if (!giver)
        {
            for (auto const& pair : bot->GetMap()->GetGameObjectBySpawnIdStore())
            {
                GameObject* go = pair.second;
                if (!go || !go->hasQuest(questId)) continue;
                if (!bot->IsWithinDistInMap(go, INTERACTION_DISTANCE)) continue;
                giver = go;
                break;
            }
        }
        if (!giver) return false;

        bot->AddQuestAndCheckCompletion(quest, giver);

        // Report what happened rather than that we tried. This half is what kept
        // the failure invisible.
        bool const accepted = bot->GetQuestStatus(questId) != QUEST_STATUS_NONE;
        if (g_EnableOllamaBotBuddyDebug && !accepted)
        {
            LOG_INFO("server.loading",
                     "[OllamaBotBuddy] Bot {} failed to accept quest {}",
                     bot->GetName(), questId);
        }
        return accepted;
    }

    bool TurnInQuest(Player* bot, uint32 questId)
    {
        if (!bot) return false;
        
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!ai) return false;
        
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest) return false;

        // Check if quest is ready to turn in
        if (bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE || bot->GetQuestRewardStatus(questId))
        {
            if (g_EnableOllamaBotBuddyDebug)
            {
                LOG_INFO("server.loading", "[OllamaBotBuddy] Bot {} cannot turn in quest {}: status={}, already rewarded={}", 
                    bot->GetName(), questId, bot->GetQuestStatus(questId), bot->GetQuestRewardStatus(questId));
            }
            return false;
        }
        
        if (!bot->CanRewardQuest(quest, false))
        {
            if (g_EnableOllamaBotBuddyDebug)
            {
                LOG_INFO("server.loading", "[OllamaBotBuddy] Bot {} cannot reward quest {}: requirements not met", 
                    bot->GetName(), questId);
            }
            return false;
        }
        
        // Find quest giver in range
        ObjectGuid questGiverGuid;
        Map* map = bot->GetMap();
        if (map)
        {
            for (auto const& pair : map->GetCreatureBySpawnIdStore())
            {
                Creature* creature = pair.second;
                if (!creature || !bot->IsWithinDistInMap(creature, INTERACTION_DISTANCE)) continue;
                if (!creature->hasInvolvedQuest(questId)) continue;
                
                questGiverGuid = creature->GetGUID();
                break;
            }
            
            // Also check game objects
            if (!questGiverGuid)
            {
                for (auto const& pair : map->GetGameObjectBySpawnIdStore())
                {
                    GameObject* go = pair.second;
                    if (!go || !bot->IsWithinDistInMap(go, INTERACTION_DISTANCE)) continue;
                    if (!go->hasInvolvedQuest(questId)) continue;
                    
                    questGiverGuid = go->GetGUID();
                    break;
                }
            }
        }
        
        if (!questGiverGuid)
        {
            if (g_EnableOllamaBotBuddyDebug)
            {
                LOG_INFO("server.loading", "[OllamaBotBuddy] Bot {} cannot find quest giver for quest {}", 
                    bot->GetName(), questId);
            }
            return false;
        }
        
        // First, initiate quest completion dialog
        WorldPacket completePacket(CMSG_QUESTGIVER_COMPLETE_QUEST);
        completePacket << questGiverGuid << questId;
        completePacket.rpos(0);
        bot->GetSession()->HandleQuestgiverCompleteQuest(completePacket);
        
        // Handle quest rewards
        uint32 rewardIndex = 0;
        if (quest->GetRewChoiceItemsCount() > 1)
        {
            // Find the best reward using simple logic
            for (uint32 i = 0; i < quest->GetRewChoiceItemsCount(); ++i)
            {
                if (quest->RewardChoiceItemId[i])
                {
                    ItemTemplate const* item = sObjectMgr->GetItemTemplate(quest->RewardChoiceItemId[i]);
                    if (item && bot->CanUseItem(item) == EQUIP_ERR_OK)
                    {
                        rewardIndex = i;
                        break; // Use first usable reward
                    }
                }
            }
        }
        
        // Complete the reward selection
        WorldPacket rewardPacket(CMSG_QUESTGIVER_CHOOSE_REWARD);
        rewardPacket << questGiverGuid << questId << rewardIndex;
        rewardPacket.rpos(0);
        bot->GetSession()->HandleQuestgiverChooseRewardOpcode(rewardPacket);
        
        if (g_EnableOllamaBotBuddyDebug)
        {
            LOG_INFO("server.loading", "[OllamaBotBuddy] Bot {} turned in quest {}: {} with reward index {}", 
                bot->GetName(), questId, quest->GetTitle(), rewardIndex);
        }
        
        return true;
    }

    bool LootNearby(Player* bot)
    {
        if (!bot || !bot->GetMap()) return false;

        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!ai) return false;

        // Looting is a four-stage pipeline in LootNonCombatStrategy, and takeover
        // deletes that strategy along with the rest of BOT_STATE_NON_COMBAT:
        //
        //     often                -> add all loot    populate the stack
        //     loot available       -> loot            SELECT a target
        //     far from loot target -> move to loot    approach it
        //     can loot             -> open loot       actually loot it
        //
        // This function used to call only "loot", which merely selects a corpse
        // and never opens one, so a bot under takeover reported "loot did not
        // execute" in roughly 9% of captured decisions. Repopulating the stack
        // alone was tried and measured at no effect, for the same reason.
        //
        // Drive the stages the deleted strategy used to drive: find the nearest
        // corpse the bot actually has rights to, make it the loot target, and
        // open it. OpenLootAction::DoLoot enforces INTERACTION_DISTANCE - 2, so
        // only consider corpses already that close; the situation assessment only
        // mentions corpses within 6 yards, so the approach stage is not needed.
        if (AiObjectContext* ctx = ai->GetAiObjectContext())
        {
            Creature* best = nullptr;
            float bestDist = INTERACTION_DISTANCE - 2.0f;

            for (auto const& pair : bot->GetMap()->GetCreatureBySpawnIdStore())
            {
                Creature* c = pair.second;
                if (!c || !c->isDead()) continue;
                if (!c->hasLootRecipient()) continue;
                if (c->GetLootRecipient() != bot &&
                    !(c->GetLootRecipientGroup() && bot->GetGroup() == c->GetLootRecipientGroup()))
                    continue;

                float const d = bot->GetDistance(c);
                if (d < bestDist)
                {
                    best = c;
                    bestDist = d;
                }
            }

            if (best)
            {
                ObjectGuid const lootGuid = best->GetGUID();

                // Keep the stack consistent: OpenLootAction removes the guid from
                // it on success, and nothing else is adding to it any more.
                if (LootObjectStack* stack = ctx->GetValue<LootObjectStack*>("available loot")->Get())
                    stack->Add(lootGuid);

                ctx->GetValue<LootObject>("loot target")->Set(LootObject(bot, lootGuid));

                Event openEvent = Event("", "");
                if (ai->DoSpecificAction("open loot", openEvent, /*silent=*/true))
                    return true;
            }
        }

        // Fall back to the original behaviour when nothing is in reach.
        Event event = Event("", "");
        return ai->DoSpecificAction("loot", event, /*silent=*/true);
    }

} // namespace BotBuddyAI

bool HandleBotControlCommand(Player* bot, const BotControlCommand& command)
{
    if (g_EnableOllamaBotBuddyDebug && bot)
    {
        LOG_INFO("server.loading", "[OllamaBotBuddy] HandleBotControlCommand for '{}', type {}", bot->GetName(), int(command.type));
        LOG_INFO("server.loading", "[OllamaBotBuddy] ================================================================================================");
    }
    if (!bot) return false;
    switch (command.type)
    {
        case BotControlCommandType::MoveTo:
            if (command.args.size() >= 3)
            {
                float x = std::stof(command.args[0]);
                float y = std::stof(command.args[1]);
                float z = std::stof(command.args[2]);
                return BotBuddyAI::MoveTo(bot, x, y, z);
            }
            break;
        case BotControlCommandType::Attack:
            if (!command.args.empty())
            {
                uint32 lowGuid = 0;
                try {
                    lowGuid = std::stoul(command.args[0]);
                } catch (const std::invalid_argument& e) {
                    LOG_ERROR("server.loading", "[OllamaBotBuddy] Invalid argument for lowGuid '{}'", command.args[0]);
                    return false;
                } catch (const std::out_of_range& e) {
                    LOG_ERROR("server.loading", "[OllamaBotBuddy] Out of range value for lowGuid '{}'", command.args[0]);
                    return false;
                }

                // Try to find the Creature by LowGuid first
                Creature* creatureTarget = nullptr;
                for (auto const& pair : bot->GetMap()->GetCreatureBySpawnIdStore())
                {
                    Creature* c = pair.second;
                    if (!c) continue;
                    if (c->GetGUID().GetCounter() == lowGuid)
                    {
                        creatureTarget = c;
                        break;
                    }
                }

                if (creatureTarget)
                {
                    // Use the actual GUID from the creature, never reconstruct!
                    return BotBuddyAI::Attack(bot, creatureTarget->GetGUID());
                }

                // If not found, try Player
                ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(lowGuid);
                Player* playerTarget = ObjectAccessor::FindConnectedPlayer(guid);
                if (playerTarget)
                {
                    return BotBuddyAI::Attack(bot, playerTarget->GetGUID());
                }

                LOG_INFO("server.loading", "[OllamaBotBuddy] Could not find target with lowGuid {}", lowGuid);
                return false;
            }
            break;
        case BotControlCommandType::Interact:
            if (!command.args.empty())
            {
                uint32 lowGuid = 0;
                try {
                    lowGuid = std::stoul(command.args[0]);
                } catch (const std::invalid_argument& e) {
                    LOG_ERROR("server.loading", "[OllamaBotBuddy] Invalid argument for lowGuid '{}'", command.args[0]);
                    return false;
                } catch (const std::out_of_range& e) {
                    LOG_ERROR("server.loading", "[OllamaBotBuddy] Out of range value for lowGuid '{}'", command.args[0]);
                    return false;
                }
                Creature* creatureTarget = nullptr;
                GameObject* goTarget = nullptr;

                // Find creature by LowGuid
                for (auto const& pair : bot->GetMap()->GetCreatureBySpawnIdStore())
                {
                    Creature* c = pair.second;
                    if (!c) continue;
                    if (c->GetGUID().GetCounter() == lowGuid)
                    {
                        creatureTarget = c;
                        break;
                    }
                }

                if (creatureTarget)
                {
                    return BotBuddyAI::Interact(bot, creatureTarget->GetGUID());
                }

                // Find gameobject by LowGuid
                for (auto const& pair : bot->GetMap()->GetGameObjectBySpawnIdStore())
                {
                    GameObject* go = pair.second;
                    if (!go) continue;
                    if (go->GetGUID().GetCounter() == lowGuid)
                    {
                        goTarget = go;
                        break;
                    }
                }

                if (goTarget)
                {
                    return BotBuddyAI::Interact(bot, goTarget->GetGUID());
                }

                LOG_INFO("server.loading", "[OllamaBotBuddy] Could not find interact target with lowGuid {}", lowGuid);
                return false;
            }
            break;
        case BotControlCommandType::CastSpell:
            if (!command.args.empty())
            {
                uint32 spellId = 0;
                try {
                    spellId = std::stoi(command.args[0]);
                } catch (const std::invalid_argument& e) {
                    LOG_ERROR("server.loading", "[OllamaBotBuddy] Invalid argument for spellId '{}'", command.args[0]);
                    return false;
                } catch (const std::out_of_range& e) {
                    LOG_ERROR("server.loading", "[OllamaBotBuddy] Out of range value for spellId '{}'", command.args[0]);
                    return false;
                }
                Unit* target = nullptr;
                if (command.args.size() > 1)
                {
                    uint32 lowGuid = 0;
        try {
            lowGuid = std::stoul(command.args[1]);
        } catch (const std::invalid_argument& e) {
            LOG_ERROR("server.loading", "[OllamaBotBuddy] Invalid argument for lowGuid '{}'", command.args[1]);
            return false;
        } catch (const std::out_of_range& e) {
            LOG_ERROR("server.loading", "[OllamaBotBuddy] Out of range value for lowGuid '{}'", command.args[1]);
            return false;
        }
                    // Try to find creature by lowGuid
                    for (auto const& pair : bot->GetMap()->GetCreatureBySpawnIdStore())
                    {
                        Creature* c = pair.second;
                        if (c && c->GetGUID().GetCounter() == lowGuid)
                        {
                            target = c;
                            break;
                        }
                    }
                    // Try to find player by lowGuid if not found
                    if (!target)
                    {
                        ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(lowGuid);
                        Player* playerTarget = ObjectAccessor::FindConnectedPlayer(guid);
                        if (playerTarget) target = playerTarget;
                    }
                }
                else
                {
                    target = bot; // Use bot itself as the target if no guid provided
                }
                return BotBuddyAI::CastSpell(bot, spellId, target);
            }
            break;
        case BotControlCommandType::Say:
            if (!command.args.empty())
            {
                return BotBuddyAI::Say(bot, command.args[0]);
            }
            break;
        case BotControlCommandType::Follow:
            return BotBuddyAI::FollowMaster(bot);
        case BotControlCommandType::Stop:
            return BotBuddyAI::StopMoving(bot);
        case BotControlCommandType::AcceptQuest:
            if (!command.args.empty())
            {
                uint32 questId = std::stoi(command.args[0]);
                return BotBuddyAI::AcceptQuest(bot, questId);
            }
            break;
        case BotControlCommandType::TurnInQuest:
            if (!command.args.empty())
            {
                uint32 questId = std::stoi(command.args[0]);
                return BotBuddyAI::TurnInQuest(bot, questId);
            }
            break;
        case BotControlCommandType::Loot:
            return BotBuddyAI::LootNearby(bot);
        default:
            break;
    }
    return false;
}


bool ParseBotControlCommand(Player* bot, const std::string& commandStr)
{
    if (g_EnableOllamaBotBuddyDebug && bot)
    {
        LOG_INFO("server.loading", "[OllamaBotBuddy] ParseBotControlCommand for '{}': {}", bot->GetName(), commandStr);
    }
    std::istringstream iss(commandStr);
    std::string cmd;
    iss >> cmd;
    if (cmd == "move")
    {
        std::string to;
        iss >> to;
        if (to != "to") return false;
        float x, y, z;
        iss >> x >> y >> z;
        BotControlCommand command = {BotControlCommandType::MoveTo, {std::to_string(x), std::to_string(y), std::to_string(z)}};
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    else if (cmd == "attack")
    {
        std::string guid;
        iss >> guid;
        BotControlCommand command = {BotControlCommandType::Attack, {guid}};
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    else if (cmd == "interact")
    {
        std::string guid;
        iss >> guid;
        BotControlCommand command = {BotControlCommandType::Interact, {guid}};
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    else if (cmd == "say")
    {
        std::string msg;
        std::getline(iss, msg);
        BotControlCommand command = {BotControlCommandType::Say, {msg}};
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    else if (cmd == "loot")
    {
        BotControlCommand command = {BotControlCommandType::Loot, {}};
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    else if (cmd == "follow")
    {
        BotControlCommand command = {BotControlCommandType::Follow, {}};
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    else if (cmd == "stop")
    {
        BotControlCommand command = {BotControlCommandType::Stop, {}};
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    else if (cmd == "acceptquest")
    {
        uint32 questId;
        iss >> questId;
        BotControlCommand command = {BotControlCommandType::AcceptQuest, {std::to_string(questId)}};
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    else if (cmd == "turninquest")
    {
        uint32 questId;
        iss >> questId;
        BotControlCommand command = {BotControlCommandType::TurnInQuest, {std::to_string(questId)}};
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    else if (cmd == "spell")
    {
        uint32 spellId;
        iss >> spellId;
        std::string targetGuid;
        iss >> targetGuid;
        BotControlCommand command;
        if (!targetGuid.empty())
        {
            command = {BotControlCommandType::CastSpell, {std::to_string(spellId), targetGuid}};
        }
        else
        {
            command = {BotControlCommandType::CastSpell, {std::to_string(spellId)}};
        }
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    return false;
}

std::string FormatCommandString(const BotControlCommand& command)
{
    std::ostringstream ss;
    switch (command.type)
    {
        case BotControlCommandType::MoveTo:
            ss << "move to";
            for (const auto& arg : command.args)
                ss << " " << arg;
            break;
        case BotControlCommandType::Attack:
            ss << "attack";
            for (const auto& arg : command.args)
                ss << " " << arg;
            break;
        case BotControlCommandType::Interact:
            ss << "interact";
            for (const auto& arg : command.args)
                ss << " " << arg;
            break;
        case BotControlCommandType::CastSpell:
            ss << "cast";
            for (const auto& arg : command.args)
                ss << " " << arg;
            break;
        case BotControlCommandType::Loot:
            ss << "loot";
            break;
        case BotControlCommandType::Follow:
            ss << "follow";
            break;
        case BotControlCommandType::Say:
            ss << "say";
            for (const auto& arg : command.args)
                ss << " " << arg;
            break;
        case BotControlCommandType::AcceptQuest:
            ss << "acceptquest";
            for (const auto& arg : command.args)
                ss << " " << arg;
            break;
        case BotControlCommandType::TurnInQuest:
            ss << "turninquest";
            for (const auto& arg : command.args)
                ss << " " << arg;
            break;
        case BotControlCommandType::Stop:
            ss << "stop";
            break;
        default:
            ss << "unknown command";
            for (const auto& arg : command.args)
                ss << " " << arg;
            break;
    }
    return ss.str();
}


