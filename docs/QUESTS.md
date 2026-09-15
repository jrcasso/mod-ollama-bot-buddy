# Quest state, measured (and a correction)

## Read the status column correctly

`character_queststatus.status` uses AzerothCore's QuestStatus enum, verified in
`src/server/game/Quests/QuestDef.h`:

    QUEST_STATUS_NONE       = 0
    QUEST_STATUS_COMPLETE   = 1
    QUEST_STATUS_INCOMPLETE = 3
    QUEST_STATUS_REWARDED   = 6   (not used in DB)

**COMPLETE is 1 and INCOMPLETE is 3**, which is the opposite of the intuitive
ordering. Commit 88f1d77 got this backwards and drew two wrong conclusions from
it. Both are corrected below.

## Corrected numbers (500 online bots)

| | count |
|---|---|
| truly incomplete (status 3) | 998 |
| — requires killing an NPC or using a GO | 304 (30%) |
| — requires collecting items | 668 (67%) |
| complete, awaiting turn-in (status 1) | 228, held by 148 bots |

Two retractions of what 88f1d77 claimed:

* "only 9 of 262 incomplete quests require killing an NPC" was measuring
  **completed** quests, which by definition have no outstanding objective. The
  real figure is 304 of 998, or 30%.
* "roughly 97% of the quests these bots hold get no targeting support" followed
  from that error. The genuinely unsupported category is item collection, 668 of
  998, or 67%.

So the quest-target branch added in 88f1d77 covers a substantially larger share
of quests than that commit credited it with.

## Quest progression is not broken

    character_queststatus_rewarded   5,724 rows across 490 bots (median ~18 each)
    online characters                average level 44.5, min 1, max 80

Bots do accept, complete and hand in quests, and they level. The 228 completed
quests awaiting turn-in are ordinary churn, roughly 1.5 per affected bot with a
long tail of one bot holding 5, not a stuck backlog. Do not "fix" turn-ins.

## Why the quest-target situation still never fires

Across 60 further captured prompts with 20 bots sampled, the entity list tagged
a quest target zero times. It is not for want of quests: five of the sampled
bots held incomplete kill quests at that moment. They simply were not standing
near the creature their quest required.

That is worth sitting with, because those 5,724 rewards come from mod-playerbots'
own AutoDoQuests, running on the ~495 bots this module does not control. A bot
under LLM control has its movement replaced by whatever the decision loop picks,
and with no situation line the loop picks move_to 24 times out of 24
(docs/PROMPT-SIZE.md). It wanders instead of travelling to its objective.

The implication is that LLM takeover may make a bot *worse* at questing than the
native AI it replaces. This has not been measured and should not be asserted
without measuring it. A fair test would compare quest reward rate for the same
bots with and without takeover over a fixed window, which needs the takeover set
held stable for long enough to accumulate rewards.

## Do not measure questing with character_queststatus_rewarded

An A/B run of 31 LLM-controlled bots against 469 native ones over 30 minutes
appeared to show the native AI completing quests 7.6 times faster. That number is
an artifact and should not be repeated.

115 of the 116 rewards came from just 7 bots, all of which had an identical
`leveltime` of about 901 seconds and five of which were offline by the end of the
window. That is `RandomPlayerbotMgr` rotating bots between level brackets, with
`PlayerbotFactory` granting quest history in bulk as part of re-randomising a
character. It is not gameplay.

With those seven excluded, the taken-over group produced one genuine turn-in
(Evelista, level 3) and the native group produced none, which points the other
way and is equally meaningless at that sample size. Counting bots that gained
anything rather than totals: 1 observed among the taken-over bots against 0.46
expected from the native rate, so no deficit is detectable either.

**The question of whether takeover harms play remains open.** A valid design needs
a metric immune to rotation: exclude any bot whose level or `leveltime` changed
during the window, or measure something behavioural from the server log instead
of a cumulative DB counter. It also needs a longer window, because organic
turn-ins are rare enough that 30 minutes over 500 bots yielded a single one.

## Looting under takeover is a four-stage pipeline, and we only call stage two

`LootNearby` calls `ai->DoSpecificAction("loot", ...)`, which fails for bots under
takeover. 29 of 336 captured prompts, 8.6%, reported "loot did not execute".

`LootAction::Execute` returns false unless `AI_VALUE(bool, "has available loot")`,
and that is

    !AI_VALUE(bool, "can loot") && available_loot->CanLoot(lootDistance)

The stack behind `available loot` is a `ManualSetValue`: it does not compute
itself, something has to `Add()` to it. Normally `LootNonCombatStrategy` does,
and that strategy lives in the non-combat engine, which takeover deletes with
`ClearStrategies(BOT_STATE_NON_COMBAT)`.

Filling the stack from the module was tried and **did not work**: the failure rate
went 8.6% to 10.1%, i.e. unchanged. The reason is that `LootNonCombatStrategy`
runs four separate stages, and only the second is what `LootNearby` invokes:

    often                  -> add all loot     populate the stack
    loot available         -> loot             SELECT a target   <- the only one we call
    far from loot target   -> move to loot     approach it
    can loot               -> open loot        actually loot it

So `"loot"` only ever *selects* a corpse; `"open loot"` is what loots it. That also
explains the `!can loot` inversion above: selection is gated off once a target
exists.

A real fix has to drive the whole pipeline from the module: pick the nearest
corpse the bot has loot rights to, set it as the loot target, and call
`"open loot"` when inside `INTERACTION_DISTANCE - 2`. The situation assessment
already only mentions corpses within 6 yards, so the approach stage is usually
unnecessary. That was not attempted here because it is a larger change than one
measured variable.


## Collection quests now have targeting (iteration 47)

Two thirds of the quests these bots hold are collection quests -- 668 of 998
incomplete quests need items, against 304 needing an NPC killed or used. Until now
only the latter had any targeting, because the assessment matched on
`RequiredNpcOrGo`. A bot carrying "collect 10 hides" would stand beside the
creature that drops them with nothing saying so.

`LootStore::HaveQuestLootForPlayer(lootid, player)` answers this directly against
an in-memory template map, resolving reference entries and per-player quest state
itself, so there is no reverse index to build and no query to run.

It is asked **once per creature, not once per quest**. The call is a question
about the player rather than about one quest, so putting it inside the per-quest
loop would have attributed the drop to whichever quest the loop happened to be on
-- a wrong title attached to a correct action.

Verified with 14 bots over eight minutes, with DeterministicActions temporarily
off so the assessment's output reaches the captured prompts:

    quest-target situations   0 of ~65 prompts before
                              2 of 67 prompts after, both from the new path

Still only 3% of decisions, because it needs the bot to be standing near a
creature that drops what it wants. But it fires where nothing fired before.
