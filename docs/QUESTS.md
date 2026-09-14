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
