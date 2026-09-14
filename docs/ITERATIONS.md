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
| 30 | Efficacy | Drive the full loot pipeline (`open loot`), not just *select* | Module called only stage 2 of 4, so it could never actually loot | 30 bots, ~20 min | **Unverified — never exercised.** 0 dead creatures appeared in ~900 captured prompts, so the path never ran. Kept: it completes a real gap, and only fires when a corpse is in range |
| 31 | Efficacy | Gate `loot` out of the schema when no corpse is in range; **reverted** | The model chose `loot` with nothing lootable in 6-9% of decisions | 30 bots, ~20 min, failure mix | **Target metric fixed, total made worse.** loot-fail 8.6% → **0%**, but interact-fail 11.3% → **34.9%** and all-fail 21.7% → **36.0%**. Blocking one invalid action displaced it into a worse one |

## Recurring lessons

* **Position beats wording.** Three separate fixes were the same fix: move the
  instruction to the end of the prompt. 0/66→66/66, 0/18→15/15, 0/16→16/16.
* **Measure against captured prompts.** Synthetic prompts inverted three results.
* **Check the marker before trusting a grep.** `- HOSTILE:` vs `ENEMY:` and
  `QUEST TARGET` vs `[QUEST TARGET -` each produced a wrong conclusion.
* **Behaviour never sampled is behaviour never verified.** Combat and grouping
  were both invisible until the sampling was fixed.
* **Constraining the model displaces the error, it does not remove it.** Gating
  `loot` out of the schema drove loot failures to zero and interact failures from
  11% to 35%. Measure the total, not the metric you aimed at.
* **We are re-implementing deterministic AI that already worked.** Takeover deletes
  the non-combat brain, and nearly every fix since has been rebuilding one of its
  pieces (accept quest, loot pipeline, quest targeting, fight-back) so a 7B model
  can be asked to agree with an answer the code already computed. See the note at
  the end of this file.
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
| 32 | Efficacy | Config knob `ClearNonCombatStrategies`, and measure keeping the deterministic non-combat brain | Takeover deletes mod-playerbots' own questing/travel/loot AI, which works on the 490 bots we don't control | 30 bots, ~20 min, vs a same-era CLEAR=1 control | **A wash.** walked 293y → 420y (+43%), frozen 4/25 → 6/29, failures 36.0% → 38.4%, quest accepts 4 → 5 bots. Keeping the brain does not help because the LLM overrides it. Default left at 1 |
| 33 | Efficacy | Execute the situation assessment directly (`DeterministicActions`, default on) instead of asking the model to agree | Standing principle: deterministic first, LLM for speech and open-ended choices. The assessment already knows the command; a 24k prompt and ~21s wait bought 0% compliance until moved to the prompt's end | Same binary, config flip only, 30 bots x ~20 min each arm | **Mechanism verified, benefit not yet demonstrated.** SITUATION reaching the model 9.1% → **1.2%** (~87% intercepted, no inference). frozen 3/26 → 2/26, walked 420y → 403y, failures 33.7% → 30.6% — all within noise. Kept: removes inference where the answer is known |
| 34 | Efficacy | Retest `ClearNonCombatStrategies = 0` now that situations are handled deterministically; **no default change** | `clear=0` alone was a wash because the LLM overrode travel every tick; with `det=1` the native travel AI should have room | Same binary, config flips, 4 arms x ~20 min, winning arm replicated | **Did not replicate.** run 1 496y/27.0%, run 2 425y/31.9% vs default 403y/30.6%. Variance ≈ effect size, so the run-1 win was noise. Default stays `clear=1` |
| 35 | Humanity | Let human movement run in battlegrounds, and allow hop/emote (never facing changes) in combat | User: players constantly move while fighting; bots were blanket-excluded from quirks in combat and BG/arena, so they were statues exactly where players move most | 10-min regression run, no LLM needed | **No regression:** 32 rewards/10min vs 36 baseline, corpses 8→12, 0 crashes, tick 104-126ms, CPU 300%. BG idle fidgeting now runs (was blanket-blocked). In-combat firing **verified reachable in row 36** (88.6% pass the idle guard) |
| 36 | Humanity | Verify the in-combat hop is reachable; **retracts row 35's caveat** | Row 35 shipped combat fidgeting but flagged that the `IDLE_MOTION_TYPE` guard might block it | Temporary debug counter, 5 min, gate opened with zero bots selected so no inference ran | **Reachable: 88.6%** of in-combat observations pass the idle guard (163,843 sampled; 145,210 IDLE). Also found `ApplyHumanMovement` is behind the `RequirePlayerOnline` early-return, so it runs **only when a real player is online** — which is why row 35 could not be verified |
| 37 | Efficacy / Humanity | Deterministic combat strafe (`CombatStrafe`, default on): step sideways preserving range band, LoS and cast state | Row 36 measured bots standing still for 88.6% of in-combat observations — the stillness the user described. Ranges/LoS/cast state are all server-side, so this is code, not inference | Same binary, config flip, 10 min per arm, gate opened with 0 bots selected so no model loaded | **No combat regression:** bot deaths 3 vs 3, tick 152→104ms, CPU 292→303%, 0 crashes. Strafe **verified firing**: ~93% of attempts clear all three guards (tried 132, fired 125 over 4 min) |
| 38 | Humanity | Say the model's text via `PlayerbotAI::Say` instead of a playerbot action that discards it | `SayAction::Execute(Event /*event*/)` ignores its parameter — it emits canned qualifier-keyed chatter, not arbitrary text. Every generated line was dropped, silently. Speech is the LLM's only measured value-add | 12 bots, 8 min, debug-gated log of what each bot says | **Speech works: 88 say lines from 89 LLM replies.** Bots were previously silent from this module. **New problem visible:** the text is near-identical robotic narration ("Checking if there are any quests available") on nearly every decision |

## Architectural direction (decided): deterministic first, LLM for speech

The user's instruction, 2026-09-14: *"use deterministic approaches in general, and
rely on LLM for speech and genuinely open-ended decisions."* This is now the
standing principle for this module, not an open question.

`ClearStrategies(BOT_STATE_NON_COMBAT)` deletes the native brain that quests,
travels, loots and vendors. That brain demonstrably works: 5,724 quest turn-ins
across 490 bots this module does not control, average level 44.5.

What replaced it costs ~0.2 decisions/second and ~21s per decision, and picks
move_to 24 times out of 24 when no situation line applies. Meanwhile
BuildSituationAssessment already computes the correct action deterministically --
corpse underfoot, attacker on you, giver in reach, objective visible -- and then
we serialise it into a 24,000 character prompt and ask the model to echo it back.
Compliance with those instructions was 0% until they were moved to the end of the
prompt.

The one place the LLM measurably adds something no rule could is speech: personas
produce visibly distinct chat (docs/PERSONAS.md), while showing no effect on
actions across four tests.

The first step was tried and measured (row 32): simply *keeping* the non-combat
brain does not work, because the LLM still issues the movement commands that
override it. Walked distance rose 43% and everything else was unchanged.

So the change that matters is the other half: **execute the situation assessment
directly instead of asking the model to agree with it.** When the assessment has a
definite answer -- corpse underfoot, attacker on you, giver in reach, objective
visible -- the module already knows the correct command and does not need
inference to produce it. Reserve the model for speech, which is the one thing it
measurably does that no rule can (docs/PERSONAS.md), and for situations where the
assessment produces nothing.

Row 33 implemented this and measured it. The mechanism works -- situation-bearing
decisions reaching the model fell from 9.1% to 1.2% -- but no behavioural gain was
detectable, because only about 9% of ticks have a definite answer today. The lever
now is **widening what the assessment can decide**, not the plumbing.

Originally expected, still to be demonstrated: the ~21s decision latency
disappears from the common path, the 36% action-failure rate should collapse
since the module only issues commands it has already validated, and inference
capacity (~0.2 decisions/second) stops being the limit on how many bots can be
driven.
