# Measuring prompt changes

**Measure against captured prompts, never hand-written ones.** Three of the four
adherence claims this module once carried were produced by a synthetic harness
and were false in production. The reason is simple: a hand-written eval prompt is
a few hundred characters, and a real one is ~24,000. An instruction that a short
prompt obeys perfectly can be ignored 100% of the time once it is buried.

## How to capture

`tools/capture_prompts.py` is a reverse proxy that sits between the module and
Ollama and logs every request. No rebuild is needed, because
`OllamaBotControl.Url` is a config value:

    CAP=/tmp/cap.jsonl python3 tools/capture_prompts.py &

    # in mod_ollama_bot_buddy.conf
    OllamaBotControl.Url               = http://host.docker.internal:11435/api/generate
    OllamaBotControl.RequirePlayerOnline = 0
    OllamaBotControl.TestBotCount        = 3

Restart the worldserver, wait for rows, then **put the config back**. Leaving
`RequirePlayerOnline = 0` on pins a 4.3GB model and has caused OOM kills twice.

Then replay with `tools/eval_situations.py`, which strips the SITUATION block out
of real prompts, injects one situation at a time, and compares placements.

## What this found

Position of the situation block, identical wording in both arms, replayed against
real captured prompts:

| Situation | block at end | block at top |
|---|---|---|
| low HP, defensive available | 12/12 | 12/12 |
| low HP, no defensive | 12/12 | 12/12 |
| lootable corpse in reach | 12/12 | **0/12** |
| quest turn-in in reach | 12/12 | **0/12** |
| quest giver in reach | **66/66** | **0/66** |

The quest-giver row is two independent captures, before and after the change.

Three consequences worth keeping:

1. **Recency beats emphasis.** The block used to sit near the top specifically so
   it would be read before the movement instructions. That reasoning was exactly
   backwards. Suppressing the destination menu instead does not help (0/12), so
   this is position, not menu pressure.

2. **Urgency partly survives burial.** Both low-HP rows pass at either position.
   A survival instruction holds where a "loot this" instruction does not, so a
   green result on one situation type says nothing about the others. Measure each.

3. **The synthetic numbers were not merely noisy, they were inverted.** Lootable
   corpse and turn-in were both reported 12/12 by the old harness while being
   0/12 in production. Treat any adherence figure without a captured-prompt
   provenance as unverified.

## Cost

A replay of 4 situations x 2 placements x 4 base prompts x 3 samples is about 96
requests, roughly 5 minutes. Inference capacity is ~0.2 decisions/second (see
docs/INFERENCE-CAPACITY.md), so keep sample counts modest and vary ONE thing.

## A monitoring trap introduced by the speech log

Iteration 38 added a debug line that prints what each bot says. That text is now
model-generated and varied, which means a crash check written as

    grep -ciE 'crash|ASSERTION|SIGSEGV' Server.log

can match a bot's dialogue. During iteration 42 that check returned 16 on a log
that showed `restarts=0`, no `Aborted`, no `Segmentation fault`, no `core dumped`,
and a server that never went down. The log rotated on restore before the lines
could be read, so what they actually were is **unknown** -- bot speech is the
likely explanation but it was not verified.

**Resolved (2026-09-15), and the exclusion above was not enough.** The same
false alarm recurred at 26 lines. The cause is game data: creature names in this
expansion include "Crash Bigbomb", "Crashed Recon Pilot", "Crashing Wave-Spirit"
and "Crashin' Thrashin' Racer". They reached Server.log through a module log line
that lacked the `[OllamaBotBuddy]` prefix and dumped raw LLM JSON, so the
exclusion did not catch it. That line is now prefixed like every other.

Do not grep Server.log for crashes at all. Use the authoritative sources:

    docker exec ac-worldserver sh -c 'wc -c < /azerothcore/env/dist/logs/Errors.log'
    docker inspect -f '{{.RestartCount}}' ac-worldserver
    grep -icE 'Aborted|terminate called|Segmentation fault|core dumped' Server.log

`Errors.log` was 0 bytes through both incidents, restart count stayed 0, and no
fatal marker ever appeared. Those three agreed with each other and with reality;
the keyword grep did not.
