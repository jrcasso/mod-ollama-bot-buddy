#!/usr/bin/env bash
#
# stack.sh — one-shot control for the AzerothCore + playerbots + Ollama stack.
#
#   ./stack.sh start        bring everything up (Docker Desktop, ollama, containers)
#   ./stack.sh stop         graceful shutdown of the servers, unload the model,
#                           stop ollama serve
#   ./stack.sh stop --full  same, and quit Docker Desktop too (the big power saving)
#   ./stack.sh status       what is running right now
#   ./stack.sh restart      stop then start
#
# Graceful shutdown: `docker compose stop` sends SIGTERM, which worldserver's
# boost signal_set (src/server/apps/worldserver/Main.cpp:236) turns into
# World::StopNow — the normal clean exit path that saves characters and closes
# the DB connections. It is NOT a kill. The default 10s compose timeout is too
# short for 500 bots to save, so we pass -t $GRACE (default 300s).
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
CONF="$REPO/env/dist/etc/modules/mod_ollama_bot_buddy.conf"
GRACE="${GRACE:-300}"

cd "$REPO"

say() { printf '\033[36m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[33m!!  %s\033[0m\n' "$*"; }

model() { sed -n 's/^OllamaBotControl\.Model *= *//p' "$CONF" | head -1; }

docker_up() { docker info >/dev/null 2>&1; }

wait_for() { # wait_for <seconds> <description> <command...>
  local timeout=$1 what=$2; shift 2
  local i=0
  while ! "$@" >/dev/null 2>&1; do
    i=$((i+1))
    [ "$i" -ge "$timeout" ] && { warn "timed out waiting for $what"; return 1; }
    sleep 1
  done
  say "$what ready (${i}s)"
}

world_port_open() { nc -z 127.0.0.1 8085; }

check_conf() {
  # These two have caused OOM kills when left flipped by a capture run.
  local rpo tbc
  rpo=$(sed -n 's/^OllamaBotControl\.RequirePlayerOnline *= *//p' "$CONF" | head -1)
  tbc=$(sed -n 's/^OllamaBotControl\.TestBotCount *= *//p' "$CONF" | head -1)
  [ "${rpo:-1}" = "1" ] || warn "RequirePlayerOnline = $rpo (bots will think even with nobody watching)"
  [ "${tbc:-0}" = "0" ] || warn "TestBotCount = $tbc (a capture run left this set)"
}

cmd_start() {
  check_conf

  if ! docker_up; then
    say "starting Docker Desktop"
    docker desktop start >/dev/null
    wait_for 120 "docker daemon" docker info
  fi

  if ! pgrep -qf 'ollama serve'; then
    say "starting ollama serve"
    nohup ollama serve >/tmp/ollama-serve.log 2>&1 &
    wait_for 30 "ollama API" curl -fsS http://127.0.0.1:11434/api/tags
  fi

  say "loading model $(model)"
  curl -fsS http://127.0.0.1:11434/api/generate \
    -d "{\"model\":\"$(model)\",\"prompt\":\"hi\",\"stream\":false,\"keep_alive\":\"30m\"}" >/dev/null

  say "docker compose up -d"
  docker compose up -d

  wait_for 600 "worldserver (port 8085)" world_port_open
  cmd_status
}

cmd_stop() {
  local full=0
  [ "${1:-}" = "--full" ] && full=1

  if docker_up; then
    # Worldserver first and alone: it must flush characters to a DB that is
    # still up. Only then the auth server and the database.
    say "stopping worldserver gracefully (SIGTERM, up to ${GRACE}s to save)"
    docker compose stop -t "$GRACE" ac-worldserver
    say "stopping authserver and database"
    docker compose stop -t 60 ac-authserver ac-database
  else
    warn "docker daemon not running, nothing to stop"
  fi

  if curl -fsS http://127.0.0.1:11434/api/tags >/dev/null 2>&1; then
    # Unload first so the runner exits cleanly and releases its unified-memory
    # allocation, then stop the daemon itself.
    say "unloading model $(model)"
    curl -fsS http://127.0.0.1:11434/api/generate \
      -d "{\"model\":\"$(model)\",\"keep_alive\":0}" >/dev/null || true
    say "stopping ollama serve"
    pkill -f 'ollama serve' || true
    pkill -f 'ollama runner' || true
  else
    say "ollama already down"
  fi

  if [ "$full" = 1 ]; then
    say "quitting Docker Desktop"
    docker desktop stop >/dev/null 2>&1 || true
  fi

  cmd_status
}

cmd_status() {
  say "containers"
  docker_up && docker compose ps --format 'table {{.Name}}\t{{.Status}}' || echo "  docker daemon down"
  say "ollama"
  if curl -fsS http://127.0.0.1:11434/api/tags >/dev/null 2>&1; then
    ollama ps
  else
    echo "  not running"
  fi
  say "host"
  uptime
  [ -f "$CONF" ] && grep -E '^OllamaBotControl\.(RequirePlayerOnline|TestBotCount)' "$CONF"
}

case "${1:-status}" in
  start)   cmd_start ;;
  stop)    shift; cmd_stop "${1:-}" ;;
  restart) cmd_stop; cmd_start ;;
  status)  cmd_status ;;
  *) sed -n '3,12p' "$0"; exit 1 ;;
esac
