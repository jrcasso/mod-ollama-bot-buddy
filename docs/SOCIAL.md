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
