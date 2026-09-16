#ifndef MOD_OLLAMA_BOT_BUDDY_TELEMETRY_H
#define MOD_OLLAMA_BOT_BUDDY_TELEMETRY_H

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

class Player;

// Gameplay telemetry: enough of a record to reconstruct a moment afterwards.
//
// What existed before this was nine LOG_INFO lines, seven of them behind a
// debug flag, written as prose into Server.log among everything else, truncated
// on restart, with nothing tying a prompt to the reply it produced, the command
// that came out, or whether that command worked. Diagnosing four separate live
// defects from it meant grepping 1.3MB of log and reading source, and the
// marker chosen to do the grepping was wrong five times.
//
// The shape of the problem is that a single decision is spread across four
// places and nothing joins them. So the unit here is a TURN: an id minted when
// a decision starts and carried through every event it causes.
//
//     turn 8142  prompt   situation="lootable corpse 4y", destinations=9
//     turn 8142  reply    latency=18.4s, command=loot
//     turn 8142  command  type=loot
//     turn 8142  outcome  ok=false, detail="loot did not execute"
//
// Four lines, one key, and the answer to "why did it just stand there".
//
// Written as JSONL, one file per server run, never truncated, one object per
// line so it can be read with the same Python the capture tooling already uses.
//
// Two rules this must not break:
//   * The world thread never touches the disk. Events go on a bounded queue
//     and a writer thread drains it. A full queue drops events and counts the
//     drops rather than blocking the server.
//   * It records the player's own session, not 500 world bots. By default only
//     bots grouped with, or near, a real player are recorded -- which is
//     exactly the situation worth revisiting, and keeps a session's file in
//     megabytes rather than gigabytes.

// Open this run's file and start the writer. Safe to call twice.
void Telemetry_Start();

// Flush what is queued and close. Called on shutdown.
void Telemetry_Stop();

bool Telemetry_Enabled();

// Should this bot's activity be recorded? Applies the scope rule above.
bool Telemetry_ShouldRecord(Player* bot);

// A fresh correlation id. Monotonic within a run.
uint64_t Telemetry_NewTurn();

// The turn a bot is currently in. BeginTurn mints one and remembers it, so the
// command and outcome events do not have to be threaded through the plan and
// prefetch machinery between them -- a bot runs its own commands on the world
// thread, so "the turn this bot is in" is well defined.
uint64_t Telemetry_BeginTurn(Player* bot);
uint64_t Telemetry_CurrentTurn(Player* bot);
void     Telemetry_ForgetBot(Player* bot);

// Record one event. `fields` is merged into the object, so callers add whatever
// the kind needs. Cheap and non-blocking; returns immediately.
void Telemetry_Event(char const* kind, Player* bot, Player* player,
                     uint64_t turn, nlohmann::json fields = nlohmann::json::object());

// Counters for `.ollamabuddy telemetry` / the status line.
struct TelemetryStats
{
    uint64_t written = 0;
    uint64_t dropped = 0;
    uint64_t queued  = 0;
    std::string path;
};
TelemetryStats Telemetry_GetStats();

#endif // MOD_OLLAMA_BOT_BUDDY_TELEMETRY_H
