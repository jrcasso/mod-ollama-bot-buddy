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
