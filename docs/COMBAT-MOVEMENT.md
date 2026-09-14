# Movement during combat

Asked by the user: can bots move while attacking, as players do in battlegrounds
to break line of sight or complicate a fight? Answered from the code, not from
behaviour, so treat the behavioural parts as unmeasured.

## What works today

`BOT_STATE_COMBAT` is deliberately never cleared by takeover, so mod-playerbots'
combat brain survives intact and does move the bot:

* `CombatStrategy` runs `reach spell` at `ACTION_HIGH`, and melee specs get
  `reach melee`, so bots close to range and back off to it.
* `FleeStrategy` fires `flee` on `panic`, `outnumbered`, and `critical health`.

So a bot in a fight is not rooted. It closes, it keeps range, and it runs when
badly hurt.

## What does not exist

**None of it is tactical.** It is rotation positioning: get in range, stay in
range, run if dying. There is no kiting, no strafing, no breaking line of sight,
no using terrain.

Three concrete reasons:

1. **The human-movement code is switched off exactly where it would matter.**
   ApplyHumanMovement, which supplies the jumping, strafing and turning that make
   a bot look like a person, begins:

       if (bot->IsInCombat()) return;
       if (bot->InBattleground() || bot->InArena()) return;

   So bots fidget while idle in the world and stand still in a battleground.

2. **The command vocabulary cannot express it.** `move_to` takes a
   `destination_index` into a list of "next to <creature>" entries plus four
   cardinal explore points. There is no way to say "strafe left", "put the pillar
   between us", or "back up while casting".

3. **LLM movement fights the combat brain for the motion slot.** `MoveTo` does

       bot->GetMotionMaster()->Clear(false);
       bot->StopMoving();
       bot->GetMotionMaster()->MovePoint(0, x, y, z);

   It does not stop the attack, but clearing the motion master discards whatever
   chase or reach generator the combat engine had installed, until the next combat
   tick puts it back.

## Where this should live

Tactical combat movement is deterministic, not open-ended. "Stay at maximum spell
range from a melee attacker", "keep an obstacle between me and the caster",
"strafe while on cooldown" are all rules with inputs the server already has:
positions, line of sight, spell ranges, cooldowns. Per the standing principle they
belong in code, and specifically in the combat engine or in a deterministic
pre-step, not in a 24,000 character prompt answered every 21 seconds -- which is
far slower than a fight moves anyway.

The cheapest first step is to stop disabling ApplyHumanMovement in battlegrounds
and combat, which would at least remove the "statue in a fight" look, and measure
whether it causes the combat brain any trouble.
