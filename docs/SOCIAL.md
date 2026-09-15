# Social behaviour

The brief for this area lists AFK spots, ganking, corpse-camping, /spit, ninja
rolls, barrens chat, LFG etiquette, bag management, auction sniping, mount
linking and duel spam outside major cities. Before iteration 43 none of it
existed. This file records what does.

## Duels (iteration 43)

An idle bot, out of combat, with another idle same-faction bot within 8 yards and
in line of sight, casts Duel (spell 7266) at it on a long jittered cooldown.

Almost none of this is our code, which is the point:

* `Spell::EffectDuel` refuses the duel if either side already has one, if the
  target ignores the caster, or **if the zone does not permit duelling**. That
  last one is why this reads as "outside the city gates" without any zone logic
  here: sanctuaries forbid duels, so it simply cannot happen in the bank.
* The target accepts through mod-playerbots' `DuelStrategy`, whose trigger is the
  incoming `duel requested` packet. That strategy lives in the **combat** engine,
  which takeover does not wipe, and `AcceptDuelAction` declines below 90% health.

So the module contributes a proximity check and a cooldown. Everything else was
already there.

### Verified, and then tuned

Sending challenges is not evidence that duels happen, so a temporary census of
players with an active duel was added to check. Duels are real.

The first rate was wrong and the census is what showed it:

    cooldown 900s     peak 112 of 500 players duelling, settling near 20
    cooldown 3600s    settles to 6-16 of 500, about 1-3%

A fifth of the realm duelling at once is not flavour, it is an epidemic, and a
bot in a duel is not questing. 3600 is the default.

### The cold start needed its own fix

Every bot was eligible the instant the server came up, because none had a cooldown
entry yet, so they all challenged at once: 80 of 500 duelling in the first minute,
decaying only as the cooldowns drifted apart. New bots now get a random position
in the cycle instead of an immediate one. That took challenges from 277 in nine
minutes to 24 in seven.

### Cost

Measured with no model loaded, since none of this involves the LLM: restarts 0,
no crashes, tick 105ms, CPU 302%, all unchanged from baseline.

## Sitting (iteration 44)

Idle bots occasionally sit, and seated bots occasionally stand back up. Cosmetic
only: the core clears the sit state on movement or damage, so it cannot strand
anyone, and it is skipped while mounted.

The motivation came out of a mount census run for a different question. Mounts
work fine -- 31 rising to 124 mounted out of ~390 eligible bots over six minutes
-- but the same census showed only 40 to 55 of 500 bots **moving** at any moment.
About 90% of the world is standing still, so what it looks like while standing
still matters.

Two mistakes were caught by checking rather than shipping:

1. **The branch was far too likely.** It was attached to the fall-through of the
   quirk roll, and the four profile weights sum to between 70 and 84, so it would
   have fired on a fifth to five sixths of every opportunity -- the "still,
   patient" profile would have sat 84% of the time. Gated to 1-in-6, with the
   remainder going back to doing nothing as before.

2. **Sitting was a one-way latch.** Nothing stands a bot back up except moving or
   being hit, and 90% of bots are stationary, so seated bots accumulate. Measured
   after five minutes: 158 seated against 342 standing and still climbing. A
   world of statues that happen to be sitting is no better than one standing to
   attention. Seated bots now stand again on a 1-in-3 roll, making it a rhythm.

The stand-up half is reasoned from the same verified roll chain rather than
separately measured, because builds had degraded to ~51 minutes each and a third
instrumented round was not worth that. Deployment is clean: restarts 0, no
crashes, no slow ticks, CPU 293%.
