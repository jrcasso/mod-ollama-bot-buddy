#include "mod-ollama-bot-buddy_telemetry.h"
#include "mod-ollama-bot-buddy_config.h"

#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Playerbots.h"
#include "World.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace
{
    // A full queue drops rather than blocking the world thread. 20k events is
    // minutes of headroom at the rates this module produces, so reaching it
    // means the writer is wedged, not merely behind.
    constexpr size_t kMaxQueued = 20000;

    std::mutex              g_mutex;
    std::condition_variable g_cv;
    std::deque<std::string> g_queue;

    std::thread       g_writer;
    std::atomic<bool> g_running{false};

    std::atomic<uint64_t> g_written{0};
    std::atomic<uint64_t> g_dropped{0};
    std::atomic<uint64_t> g_nextTurn{1};

    // Resolved once at start. The writer thread must never read a config
    // string: .reload reassigns those on the world thread, and a std::string
    // being reassigned under a reader is a use-after-free.
    std::string g_path;
    std::mutex  g_pathMutex;

    std::mutex                                 g_turnMutex;
    std::unordered_map<uint64_t, uint64_t>     g_botTurn;

    std::string TimestampedPath()
    {
        std::string dir = g_OllamaTelemetryDir;
        if (dir.empty())
            dir = "/azerothcore/env/dist/logs/telemetry";

    // Create it rather than assuming it. A missing directory would otherwise
    // disable telemetry with only a log line to say so, which is exactly the
    // silent failure this whole system exists to stop.
    {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec)
            LOG_ERROR("server.loading",
                      "[OllamaBotBuddy] telemetry: cannot create {}: {}", dir, ec.message());
    }

        if (dir.back() != '/')
            dir += '/';

        std::time_t now = std::time(nullptr);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &now);
#else
        localtime_r(&now, &tm);
#endif
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);

        return dir + "session-" + stamp + ".jsonl";
    }

    void WriterLoop(std::string path)
    {
        // Append, so a restart inside the same second cannot truncate the file
        // it just wrote.
        std::ofstream out(path, std::ios::app);
        if (!out)
        {
            LOG_ERROR("server.loading",
                      "[OllamaBotBuddy] telemetry: cannot open {} -- is the directory missing?", path);
            g_running = false;
            return;
        }

        for (;;)
        {
            std::deque<std::string> batch;
            {
                std::unique_lock<std::mutex> lock(g_mutex);
                g_cv.wait_for(lock, std::chrono::milliseconds(500),
                              [] { return !g_queue.empty() || !g_running; });

                if (g_queue.empty() && !g_running)
                    break;

                batch.swap(g_queue);
            }

            for (std::string const& line : batch)
                out << line << '\n';

            // Flushed every batch. A crash is exactly when the tail matters,
            // and this is at most twice a second on a background thread.
            out.flush();
            g_written += batch.size();
        }

        out.flush();
    }
}

void Telemetry_Start()
{
    if (!g_EnableOllamaTelemetry || g_running)
        return;

    std::string const path = TimestampedPath();
    {
        std::lock_guard<std::mutex> lock(g_pathMutex);
        g_path = path;
    }

    g_running = true;
    g_writer = std::thread(WriterLoop, path);

    LOG_INFO("server.loading", "[OllamaBotBuddy] telemetry -> {}", path);

    Telemetry_Event("session", nullptr, nullptr, 0, {
        {"scope",        g_OllamaTelemetryAllBots ? "all_bots" : "player_adjacent"},
        {"full_prompts", g_OllamaTelemetryFullPrompts},
        {"realm_uptime", sWorld->GetUptime()},
    });
}

void Telemetry_Stop()
{
    if (!g_running)
        return;

    Telemetry_Event("session_end", nullptr, nullptr, 0, {
        {"written", g_written.load()},
        {"dropped", g_dropped.load()},
    });

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_running = false;
    }
    g_cv.notify_all();

    if (g_writer.joinable())
        g_writer.join();
}

bool Telemetry_Enabled()
{
    return g_EnableOllamaTelemetry && g_running;
}

bool Telemetry_ShouldRecord(Player* bot)
{
    if (!Telemetry_Enabled() || !bot)
        return false;

    if (g_OllamaTelemetryAllBots)
        return true;

    // The point is the player's own session. A bot grouped with a real player,
    // or standing near one, is in it; the other several hundred are not.
    if (Group* group = bot->GetGroup())
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource())
                if (member != bot && !GET_PLAYERBOT_AI(member))
                    return true;

    float const radius = float(g_OllamaTelemetryPlayerRadius);
    for (auto const& pair : ObjectAccessor::GetPlayers())
    {
        Player* p = pair.second;
        if (!p || p == bot || GET_PLAYERBOT_AI(p))
            continue;
        if (p->GetMapId() == bot->GetMapId() && bot->IsWithinDist(p, radius, false))
            return true;
    }

    return false;
}

uint64_t Telemetry_NewTurn()
{
    return g_nextTurn++;
}

uint64_t Telemetry_BeginTurn(Player* bot)
{
    if (!bot) return 0;

    uint64_t const turn = g_nextTurn++;
    std::lock_guard<std::mutex> lock(g_turnMutex);
    g_botTurn[bot->GetGUID().GetRawValue()] = turn;
    return turn;
}

uint64_t Telemetry_CurrentTurn(Player* bot)
{
    if (!bot) return 0;

    std::lock_guard<std::mutex> lock(g_turnMutex);
    auto it = g_botTurn.find(bot->GetGUID().GetRawValue());
    return it == g_botTurn.end() ? 0 : it->second;
}

void Telemetry_ForgetBot(Player* bot)
{
    if (!bot) return;

    std::lock_guard<std::mutex> lock(g_turnMutex);
    g_botTurn.erase(bot->GetGUID().GetRawValue());
}

void Telemetry_Event(char const* kind, Player* bot, Player* player,
                     uint64_t turn, nlohmann::json fields)
{
    if (!g_running)
        return;

    nlohmann::json ev = std::move(fields);

    ev["t"]    = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::system_clock::now().time_since_epoch()).count();
    ev["kind"] = kind;

    if (turn)
        ev["turn"] = turn;

    if (bot)
    {
        ev["bot"]      = bot->GetName();
        ev["bot_guid"] = bot->GetGUID().GetCounter();
        ev["map"]      = bot->GetMapId();
        ev["pos"]      = { bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ() };
    }

    if (player)
    {
        ev["player"]      = player->GetName();
        ev["player_guid"] = player->GetGUID().GetCounter();
    }

    std::string line = ev.dump();

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_queue.size() >= kMaxQueued)
        {
            ++g_dropped;
            return;
        }
        g_queue.push_back(std::move(line));
    }

    g_cv.notify_one();
}

TelemetryStats Telemetry_GetStats()
{
    TelemetryStats stats;
    stats.written = g_written.load();
    stats.dropped = g_dropped.load();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        stats.queued = g_queue.size();
    }
    {
        std::lock_guard<std::mutex> lock(g_pathMutex);
        stats.path = g_path;
    }
    return stats;
}
