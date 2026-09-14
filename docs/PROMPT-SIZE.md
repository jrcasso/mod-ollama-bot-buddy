# Prompt size is not a lever (measured, negative result)

Recorded so nobody spends another iteration on it.

## The prompt is mostly inert, and shrinking it buys nothing

A live prompt is ~24,500 characters, of which an 18,300-character static slab is
byte-identical every time. Replacing that slab with a 3-line instruction, on real
captured prompts:

| | situation adherence | prompt size |
|---|---|---|
| full prompt | 16/16 | 24,529 chars |
| slab removed | 16/16 | 6,437 chars (74% smaller) |

Identical decision quality. But **latency did not improve** (4.21s vs 4.42s
median), which is expected: prompt evaluation is already served from the
llama.cpp prefix cache at roughly 47ms, and generation dominates. See
docs/INFERENCE-CAPACITY.md.

So the slab costs almost nothing and removing it gains almost nothing, while
deleting 18,000 characters of behavioural rules that have never been tested
individually is real risk. It was left in place.

## If you ever do trim it, keep COMMUNICATION

Removing the whole slab silences the bots. On prompts with no situation line:

    full prompt      say produced 12/12
    slab removed     say produced  0/12

Retaining just the 500-character COMMUNICATION block restores it completely,
while still cutting 72%:

    trim + COMMUNICATION   situation adherence 16/16, say 12/12, 6,705 chars

Speech is the main thing personas deliver (docs/PERSONAS.md), so losing it is a
real regression, not a cosmetic one.

## The stale command documentation is wrong but harmless

The prompt documents parameter shapes that the schema does not accept:

| command | prompt says | schema requires |
|---|---|---|
| move_to | `{x, y, z}` | `destination_index` |
| attack | `{guid}` | `attack_guid` |
| accept_quest / turn_in_quest | `{id}` | `quest_id` |

There is also a COORDINATE CALCULATION RULES block, present in 132 of 138
prompts, teaching coordinate arithmetic for a command shape that cannot be
emitted. Grammar-constrained decoding forces the correct shape regardless, so
this is inert, and that was confirmed rather than assumed: removing the
coordinate rules changed nothing at all (move_to 24/24 in both arms, speech
24/24 in both). Left alone on those grounds.

## What actually moves behaviour

With no situation line the model chose move_to **24 out of 24 times**. That is
not caused by prompt bloat or by the coordinate instructions, both of which were
tested and cleared above. It is intrinsic.

The situation block is the only lever that has ever measurably changed a
decision in this module. Anything that should alter behaviour belongs there, not
in the standing instructions.

## Candidate gap, currently under-sampled

There is no situation branch for "a quest objective is visible", even though the
entity list already tags creatures with `[QUEST TARGET - <quest>]`. A bot with an
active kill quest and the target in sight gets no directive and therefore wanders.

This is only a candidate: just 3 of 83 captured prompts had a visible quest
target, and only one of those had no situation line. Worth revisiting with a
capture run aimed at questing bots before building anything.
