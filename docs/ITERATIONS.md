# Iteration log

One row per loop iteration, newest last. **Update this table at the end of every
iteration**, including iterations that shipped nothing — a measured negative
result is a result, and the ones recorded here have already prevented several
repeat investigations.

Columns: what was done, why it was worth doing, how it was verified, and what
actually came out. "Result" records the measurement, not the intention.

## Before the capture harness existed

These predate prompt capture, so their adherence numbers came from synthetic
prompts. Three of those numbers were later shown to be wrong (see PROMPT-SIZE
and PERSONAS rows). The code changes themselves stand.

| # | Area | What | Why | How verified | Result |
|---|---|---|---|---|---|
| 1 | Efficacy | Fix AzerothCore API drift, party control, structured outputs (`57724f7`) | Module would not build or steer bots | Build + live server | Bots controllable; `oneOf` schema survives Ollama's GBNF |
| 2 | Efficacy | Thread-safe LLM handling, multi-step plans, prefetch (`5c5f5b6`) | Use-after-free in detached thread; one action per inference wasted scarce capacity | Live server, no crashes | Plans amortise inference across up to 4 actions |
| 3 | Humanity | Movement quirks, personality-driven decisions (`c738c86`) | Bots moved robotically | Live | Shipped; the personality half was later disproved (row 23) |
| 4 | Efficacy | Situation assessment added (`d60d9fe`, `6f5130e`) | Bots ignored what mattered right now | Synthetic evals | Reported large gains; later found position-dependent (row 17) |
| 5 | Efficacy | Heal targeting, keep combat rotation, keep dead-state brain (`d9a2af5`, `4511a8c`, `62b893a`) | Takeover wiped the brains that fight and resurrect | Live | Bots fight with full spellbook and can resurrect |
| 6 | Efficacy | Per-command schemas (`31f1461`) | Wrong parameters were representable | Live capture | 13/13 replies carried the correct parameter |
| 7 | Tooling | `TestBotCount` (`4bc61d5`) | `BotNames` silently controls nothing when a bot rotates out | Live | Decision loop exercisable without a player |

## With captured-prompt measurement

| # | Area | What | Why | How verified | Result |
|---|---|---|---|---|---|
| 13 | Efficacy | Sort `move_to` destinations by distance, label them (`9cdee0d`) | Hash-ordered menu, duplicate labels; nearest could be truncated out | Replay, 12 samples | Picked nearest **0/12 → 12/12**; walked 82y past a target 6y away |
| 14 | LLM efficiency | Document inference capacity (`5fd22a2`) | No data on the numbers that gate scaling | Serial vs concurrent batches | **~0.2 decisions/s, GPU-bound**; 3 hypothesised bottlenecks disproved |
| 15 | Efficacy | Execute final plan step before prefetching (`b5b2376`) | Prefetch rebuilt the destination cache, so the last step resolved against a list the model never saw | Captured prompt pairs | 7 of 8 rebuilds remapped indices, 3 remapped **every** entry |
| 16 | Efficacy | Only announce quest givers with a takeable quest (`bd797b2`) | Assessment promised what the executor refused | Live capture | False offers **44% → 7%** of prompts |
| 17 | Prompt evals | Move situation block to end of prompt (`5e5b807`) | Suspected position, not wording, drove adherence | Replay, two independent captures | **0/66 → 66/66**. Synthetic harness had scored this 18/18 |
| 18 | Prompt evals | Capture/replay tooling + findings (`0056848`, `a179e85`) | The harness that produced false greens lived in /tmp | Ran from repo against real capture | Loot and turn-in were also **0/12 → 12/12**; 3 of 4 synthetic claims were inverted |
| 20 | Humanity | GUID-derived disposition for every bot (`c8fb97e`) | mod-ollama-chat writes personas lazily; 18 rows for 1,501 characters | Live capture | Coverage **10% → 100%**; mix 72% ordinary / 20% distinctive / 8% disruptive |
| 21 | Efficacy | `TestBotFilter=combat` + tell bots under attack to fight back (`43bd19c`) | Only 4% of sampled prompts were in combat, so combat was barely observed | Replay + live | In-combat sampling **4% → 90%**; fighting back **0/18 → 15/15** |
| 22 | Efficacy | Investigate why bots never group (`f55995b`) | 0 groups across 500 bots; dungeons/raids impossible | Config test on live server | Root cause: `addStrategy("group")` commented out upstream. Two config lines produce groups (0 → 3). **Left off pending a decision** |
| 23 | Humanity | Establish what personas do (`42fcc05`) | The "personality shifts decisions" claim came from the synthetic harness | 4 replay tests | Actions: **null in all four**, incl. 12/12 identical with contradictory traits. Speech: **unmistakable**. Kept for speech |
| 24 | LLM efficiency | Test whether prompt size is a lever (`e442a6e`) | 74% of the prompt is static boilerplate | Replay + latency | Identical quality at 74% smaller, but **latency unchanged** → not worth the risk. Trimming also silences bots (say 12/12 → 0/12) |
| 25 | Efficacy | Situation branch for visible quest objectives (`88f1d77`) | The existing warning sat at 18% depth, the ignored position | Replay on real prompts | **0/16 → 16/16**. *Not* observed firing in production |
| 26 | Efficacy | Correct the quest analysis (`8e21322`) | Read `status` backwards: COMPLETE=1, INCOMPLETE=3 | DB + enum in `QuestDef.h` | Retracted two claims. Kill quests are **30%**, not 3.4%. Quest progression healthy: 5,724 rewards, avg level 44.5 |
| 27 | Efficacy | A/B: does takeover make bots worse at questing? | Takeover permanently wipes the non-combat brain (`ClearStrategies(BOT_STATE_NON_COMBAT)`), which is what quests | 31 bots taken over vs 469 native, 30 min, quest rewards | **Inconclusive — metric invalid.** `character_queststatus_rewarded` is contaminated by bot re-randomisation. No evidence of harm; hypothesis still open |
| 28 | Efficacy | Accept quests through the core API instead of a playerbot action (`AcceptQuest`) | `AcceptQuestAction::Execute` returns false on its first line for a bot with no master, and callers discarded the result, so a quest never accepted looked accepted | 30 bots taken over, 15 min, `character_queststatus` deltas | **0 accepts in 30 min → 5 accepts by 4 bots in 15 min.** Live proof: a level-26 draenei had sat at Megelon for 15 decisions with 0 quest rows |
| 29 | Efficacy | Tried to repair looting under takeover by repopulating the loot stack; **reverted** | 9% of prompts reported "loot did not execute"; takeover wipes the non-combat "loot" strategy that fills the stack | 30 bots, ~25 min, failure rate vs a 336-prompt baseline | **No effect: 8.6% → 10.1%.** Looting is a 4-stage pipeline (add / select / approach / open) and the module only ever calls *select*, so filling the stack cannot help. Change reverted |

## Recurring lessons

* **Position beats wording.** Three separate fixes were the same fix: move the
  instruction to the end of the prompt. 0/66→66/66, 0/18→15/15, 0/16→16/16.
* **Measure against captured prompts.** Synthetic prompts inverted three results.
* **Check the marker before trusting a grep.** `- HOSTILE:` vs `ENEMY:` and
  `QUEST TARGET` vs `[QUEST TARGET -` each produced a wrong conclusion.
* **Behaviour never sampled is behaviour never verified.** Combat and grouping
  were both invisible until the sampling was fixed.
* **Takeover removes machinery, not just behaviour.** `ClearStrategies(BOT_STATE_NON_COMBAT)`
  deletes the strategies that populate values and drive multi-step pipelines. Two
  separate bugs (quest accept, looting) both trace to delegated playerbot actions
  whose preconditions that strategy used to satisfy.
* **A discarded return value hides a permanent failure.** `AcceptQuest` had never
  worked for a random bot, and nothing reported it, because the caller set
  `foundQuestAction = true` whatever happened.
* **Validate the metric, not just the result.** The takeover A/B produced a clean
  looking "native AI is 7.6x better" that was an artifact: 115 of 116 rewards came
  from 7 bots that `RandomPlayerbotMgr` had just re-randomised (identical
  `leveltime` of ~901s, five of them offline by the end). Quest rewards are not a
  measure of questing on this server.
