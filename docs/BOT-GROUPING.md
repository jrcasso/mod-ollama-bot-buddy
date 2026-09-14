# Bots never group with each other (and how to change that)

## The state by default

On a 500-bot server, measured directly:

    groups: 0
    group members: 0

Not "few" -- zero, and zero for the entire life of the server. Every one of 138
prompts captured from the live server reported `Group status: Solo`.

That means the whole group-coordination half of the prompt has never executed
once: the `AS TANK` / `AS HEALER` / `AS DPS` role instructions, the group member
list with per-member roles, and the healer's lowest-HP targeting are all dead
code in practice. It also means dungeons and raids are not merely untested, they
are impossible.

## Why

Two gates, and only the first is documented.

1. `AiPlayerbot.RandomBotGroupNearby` defaults to 0, and playerbots.conf notes
   "Currently not functioning properly".

2. The real reason. The action that reads that setting,
   `InviteNearbyToGroupAction`, lives in `GroupStrategy`, which is wired up
   correctly (`GroupStrategy.cpp:12` triggers "invite nearby" often). But the
   strategy is never installed, because the line that installs it is commented
   out upstream, in both places it appears:

       modules/mod-playerbots/src/Bot/Factory/AiFactory.cpp:610
       modules/mod-playerbots/src/Bot/Factory/AiFactory.cpp:643
           // nonCombatEngine->addStrategy("group");

   So setting `RandomBotGroupNearby = 1` on its own changes nothing. Verified:
   enabled it alone, restarted, waited, still 0 groups.

## Enabling it without patching mod-playerbots

`RandomBotNonCombatStrategies` is applied to every randombot after the defaults,
so the strategy can be added from config alone:

    AiPlayerbot.RandomBotGroupNearby         = 1
    AiPlayerbot.RandomBotNonCombatStrategies = "+group"

Measured on the live server with both set:

    t+0s    groups 0
    t+420s  groups 1
    t+510s  groups 2
    t+570s  groups 3   (6 bots, all pairs)

Server stayed up throughout: restarts 0, no crashes, CPU 317% versus 320% at
baseline, memory 4.90GB versus 4.65GB.

## What is still unknown

* **It is slow and small.** Three pairs out of 500 bots in ten minutes. Group
  size is capped by GrouperType, which is 20% SOLO, 60% MEMBER and 5% each of
  LEADER_2 through LEADER_5, so only about 20% of bots can lead at all and most
  of those lead pairs. This is nowhere near a five-man dungeon group yet.
* **Tick cost is not established.** Slow-tick lines appeared 3 times with
  grouping on and 0 at baseline, but the samples are far too few to attribute,
  and CPU and memory were unchanged. Worth a longer run before trusting it.
* **The group prompt path is still unverified.** Only 6 of 500 bots were
  grouped, and the TestBotCount hook adopts arbitrary bots, so a grouped bot
  would almost never be sampled. Verifying the AS TANK / AS HEALER / AS DPS text
  needs a `TestBotFilter = group` mode alongside the existing `combat` one.

These settings are left at their defaults. Turning them on changes how every bot
in the world behaves, so it is a deliberate choice rather than something to
enable silently.
