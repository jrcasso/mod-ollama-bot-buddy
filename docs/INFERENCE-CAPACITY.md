# Measured inference capacity

All numbers captured on 2026-09-14 from the live server (Apple Silicon, 18GB
unified memory, `qwen2.5:7b-instruct` via Ollama), by pointing
`OllamaBotControl.Url` at a logging reverse proxy and replaying the captured
production prompts. No rebuild was required to gather any of this.

## Headline number

**Hard capacity is ~0.2 LLM decisions per second**, and it is GPU-bound.

Serial and concurrent request batches were measured back to back:

| Mode | 3 requests, wall clock | Throughput | Per-bot wait |
|---|---|---|---|
| Serial | 14.9s | 0.20 decisions/s | 5.0s |
| Concurrent | 14.3s | 0.21 decisions/s | 9.9s |

Concurrency does **not** raise throughput; it only redistributes waiting.
Tuning `OLLAMA_NUM_PARALLEL` is therefore not worth doing.

With N LLM-controlled bots, each bot receives a plan roughly every `5N`
seconds. Observed in production with 3 test bots: ~21s per decision.

## Why multi-step plans matter

Plan length is the only real amortisation lever. At `maxItems = 4`, one
inference yields up to four actions, so a bot acts every `5N/4` seconds
instead of every `5N`. **Shortening plans would make bot responsiveness
worse, not better.**

This also means only party-sized bot counts can be LLM-controlled. That is
already the design: `ControlPartyBots = 1` puts the player's party under the
LLM while the remaining ~495 bots run the native mod-playerbots AI.

## Three things that are NOT bottlenecks

Each of these was hypothesised and then disproved by measurement:

1. **Prompt size.** Prompts are large (median 24,219 chars, 75% of which is
   static boilerplate) but prompt evaluation costs only ~47ms in steady
   state. llama.cpp prefix caching already covers it, because consecutive
   prompts from the same bot share a long prefix.

2. **Prompt ordering.** Moving the static boilerplate to the front to enlarge
   the cacheable prefix made things *worse*: 5,595ms of prompt eval on every
   request versus 48ms for the current ordering. Do not reorder the prompt
   for caching reasons.

3. **The JSON schema / grammar-constrained decoding.** The `oneOf` schema is
   ~6,000 chars, but constrained decoding is *faster* than unconstrained,
   because it cuts output from ~102 tokens to ~53:

   | | output tokens | total |
   |---|---|---|
   | Without schema | 102 | 4,568ms |
   | With `oneOf` schema | 53 | 2,616ms |

## Consequence worth remembering

Because a decision takes ~21s in production with 3 bots, the game state a
plan is based on is up to 21 seconds stale by the time the last step runs.
Situation assessment is computed fresh at request time, not at execution
time. Any future work on decision quality should weigh staleness, not
prompt wording.

`DecisionIntervalSeconds = 3` is effectively inert: inference, not the
timer, is the limiter. An in-flight `busy` flag already prevents the timer
from issuing overlapping requests for the same bot.

## Build times vary with host load, not just ccache

The standing assumption in this project was that a slow build means the ccache
was wiped (see the note about `docker builder prune`). That is not the only
cause, and on 2026-09-15 it was not the cause at all.

Two consecutive builds took about 51 minutes each where the previous ones took
50 seconds. Comparing the logs:

    fast build    1766 objects, ccache Direct 61093/61096 (100.0%), step 35s
    slow build    1766 objects, ccache Direct 64721/64724 (100.0%), step 1118s

Identical object count, identical cache behaviour, 32x slower. The cause was the
host: load average 17.4 on 12 cores, with seven concurrent Claude sessions, a
runaway macOS `Contacts` process at 90% CPU and `StorageManagement` at 30%. The
worldserver is stopped during builds, so it was not competing.

Before blaming the cache, check `uptime` and `ps aux | sort -nrk3 | head`. A
build that is slow with a 100% ccache hit rate is being starved, not recompiling.
