# What personas actually do

**Personas change how a bot talks. They do not change what it decides.**

That is the opposite of what this module used to claim. An earlier synthetic
eval reported "personality demonstrably shifts decisions (RAGER attacks 7/9,
FOOL wanders 7/9)". Measured against prompts captured from the live server, that
does not reproduce.

## Actions: four attempts, four nulls

Each used real captured prompts, not hand-written ones, and varied only the
persona block.

| Test | Result |
|---|---|
| explore vs walk-to-NPC, persona at top and at end | no differentiation |
| attack rate in combat, no situation line | 0 attacks in either arm |
| target selection, >=3 enemies, no situation line | 0 attacks in either arm |
| target selection, situation line present, persona at end | 12/12 identical in all arms |

The last row is the strongest. EDGE_LORD ("seek out the strongest enemy") and
TRICKSTER ("prefer wounded enemies") are directly contradictory traits, placed at
the end of the prompt, which is the position proven to work for situation
instructions (66/66 elsewhere). Both produced byte-identical behaviour, and
matched the no-persona arm exactly.

The common thread across the nulls: without a SITUATION directive the model picks
move_to almost regardless of input, and with one it follows the directive almost
regardless of input. A descriptive trait does not compete with either.

## Speech: unmistakable

Same prompts, same placement, looking at the `say` field:

    (none)         "Defending myself from the Rampaging Geist!"
    EDGE_LORD      "Feel the power of my blade!"
                   "Time to show some edge!"
    STONER         "Yo, this ghost is giving me some vibes, gonna handle it my way."
    NINJA_LOOTER   "Defending myself and taking what's mine."

This is where the persona earns its place, and it is the part a player actually
notices. Do not remove the block on the grounds that it fails to steer actions.

## Consequences

* The block's own line, "This shapes how you act, not just how you talk", is
  literally false. It is left alone deliberately: the trait text is what gives
  speech its colour (NINJA_LOOTER reaches for loot, EDGE_LORD for strength), and
  rewording it risks the one effect that does work, for no measured gain.
* Persona cannot be used to fix behaviour. If a bot should act differently in
  some situation, that belongs in BuildSituationAssessment, which is imperative,
  specific, and read at the end of the prompt.
* Correctness directives are safe from persona interference, which is worth
  knowing: an EDGE_LORD will not wander off to fight something bigger instead of
  defending itself.

## Speech was being discarded entirely (iteration 38)

Everything above was measured by replaying prompts and reading the model's `say`
field. None of it was reaching the game.

`BotBuddyAI::Say` called `ai->DoSpecificAction("say", Event("", msg))`, and
`SayAction::Execute` is declared

    bool SayAction::Execute(Event /*event*/)

with the parameter commented out. That action emits canned playerbot chatter
chosen by a qualifier ("low ammo" and similar) from `sPlayerbotTextMgr`; it has no
path for arbitrary text. So every line the model produced was dropped.

This is the same failure as `AcceptQuest`: delegating to a playerbot action that
cannot do the job, and not checking the result. It mattered more, because speech
is the only thing the LLM measurably contributes here.

Fixed by calling `PlayerbotAI::Say`, which selects the faction language and calls
`Player::Say`. It is what mod-ollama-chat already uses. Verified with 12 bots over
eight minutes: 88 lines spoken against 89 LLM replies.

## The text itself is poor, which is now the open problem

With speech actually reaching the game, the quality is visible for the first time:

    Trumzogg says: Checking if there are any quests available.
    Naspioshis says: Checking if there are any quests available.
    Sylrin says: Checking if there are any quests available.
    Emian says: Checking if there are any quests available.

Three faults, all separate from the plumbing:

1. ~~**It narrates the action rather than speaking.**~~ **Fixed in iteration 40**,
   by wording, not position. Adding a SPEECH block took narration from 93% to 0%
   and made the model return an empty string 57% of the time, which is what a
   player mostly does. Command selection was unchanged, so the block does not
   disturb actions.
2. **It clusters, and two attempts to fix that made things worse.**

   Iteration 40 reported this as a collapse to 8 byte-identical lines out of 9.
   That figure came from nine lines of production output and was too small to
   carry the claim. Replayed across 42 samples, the shipped block produces 11
   distinct lines out of 22, so about half. There is real clustering -- one
   phrase accounted for 9 of 22 -- but it is not the collapse first reported.

   Iteration 41 tried the obvious remedy, telling the block to speak in the voice
   of YOUR PERSONALITY, and also tried deleting the menu of options in case the
   model was simply taking the first item. Both suppress speech rather than vary
   it:

       shipped block            48% empty   11/22 distinct
       + speak as your persona  88% empty    2/5  distinct
       menu removed             95% empty    2/2  distinct

   Every instruction added to that block pushes the model further toward the
   "or nothing at all" escape hatch. Whatever fixes variety will probably not be
   another sentence in the same paragraph.

   **Iteration 42 found what does: moving the persona block itself.** It sat at
   about 1.2% depth, the same position every other instruction in this module was
   ignored at. Emitting it at the end instead, replayed on two disjoint sets of
   captured prompts:

       persona near the top   distinct 53% and 46%,  most-repeated 6x and 8x
       persona at the end     distinct 82% and 82%,  most-repeated 3x and 2x

   Confirmed in production: persona depth 1.2% to 99.4%, and eight spoken lines
   that were eight distinct lines. The register changes as well as the variety --
   "Got your back, mates!", "Let's get this over with.", "Time to take care of
   this vermin." instead of a queue of identical greetings. Command selection was
   identical in every arm, so this moves speech without touching behaviour.

   Note what this does *not* say: personas still do not change what a bot does.
   Four tests above establish that, and iteration 42 re-confirms it. Position
   affects voice, not decisions.
3. ~~**It is constant.** 88 lines from 89 decisions.~~ **Fixed in iteration 39.**
   A per-bot cooldown of 240 seconds, jittered up to double, plus a 30-yard
   earshot check, took this from 98.9% of decisions to 11.5% -- about one line per
   bot per ten minutes. Both inputs are exact server-side facts, so this is code,
   not a prompt instruction asking the model to be tastefully quiet.

The rate is fixed. The remaining two -- wording and persona carry-through -- are
genuinely prompt changes and should be measured separately against captured
prompts, one at a time.

Worth noting for whoever takes the wording: the instruction already says `"say"
must be what your character would say in-game to players`, and the model returns a
status report anyway. That is the same pattern as every other instruction in this
module that sits outside the situation block, so check where it lives in the
prompt before rewriting it.
