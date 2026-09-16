#!/usr/bin/env python3
"""Read the gameplay telemetry written by mod-ollama-bot-buddy and
mod-ollama-chat, and answer the questions you actually have after a session.

The two modules write separate JSONL files into the same directory because they
are separate repositories; this merges them by timestamp so a conversation and
the decisions around it read as one stream.

    ./telemetry.py sessions
    ./telemetry.py timeline            # everything, newest session
    ./telemetry.py timeline --bot Eram
    ./telemetry.py failures            # what did not work, most repeated first
    ./telemetry.py turn 8142           # one decision, start to finish
    ./telemetry.py around 14:32:05     # +/- 30s, all bots
    ./telemetry.py stuck               # bots that retried the same thing

Point it at a directory with --dir; the default matches the module default.
"""

import argparse
import collections
import datetime as dt
import glob
import json
import os
import sys

DEFAULT_DIR = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "../../../env/dist/logs/telemetry")


def load(directory, session=None):
    """Every event from one session (or the newest), sorted by time."""
    paths = sorted(glob.glob(os.path.join(directory, "*.jsonl")))
    if not paths:
        sys.exit(f"no telemetry in {directory}\n"
                 "Is OllamaBotControl.Telemetry on, and has the server run since?")

    if session:
        paths = [p for p in paths if session in os.path.basename(p)]
        if not paths:
            sys.exit(f"no session matching {session!r}")
    else:
        # Newest stamp wins, across both file prefixes.
        newest = max(os.path.basename(p).split("-", 1)[1] for p in paths)
        paths = [p for p in paths if p.endswith(newest)]

    events = []
    for path in paths:
        with open(path) as fh:
            for lineno, line in enumerate(fh, 1):
                line = line.strip()
                if not line:
                    continue
                try:
                    ev = json.loads(line)
                except json.JSONDecodeError:
                    # A torn last line is normal if the server was killed.
                    print(f"  (skipped unparsable {os.path.basename(path)}:{lineno})",
                          file=sys.stderr)
                    continue
                ev["_file"] = os.path.basename(path)
                events.append(ev)

    events.sort(key=lambda e: e.get("t", 0))
    return events


def clock(ev):
    return dt.datetime.fromtimestamp(ev.get("t", 0) / 1000).strftime("%H:%M:%S")


def describe(ev):
    """One line per event, dense enough to scan."""
    kind = ev.get("kind", "?")
    who = ev.get("bot", "")
    turn = f"#{ev['turn']}" if ev.get("turn") else ""

    if kind == "said_to_bot":
        return f"{ev.get('player')} -> {who}: {ev.get('text','')!r} ({ev.get('source')})"
    if kind == "bot_said":
        extra = f"  [intent: {ev['intent']}]" if ev.get("intent") else ""
        return f"{who} says: {ev.get('text','')!r}  ({ev.get('latency_ms',0)}ms){extra}"
    if kind == "intent":
        if ev.get("executed"):
            return f"{who} runs '{ev.get('command')}' for {ev.get('player')}"
        return f"{who} DID NOT run '{ev.get('command')}' ({ev.get('refused')})"
    if kind == "prompt":
        sit = "; ".join(s.replace("SITUATION: ", "") for s in ev.get("situation", []))
        return f"{who} thinking {turn}: {sit or '(no situation)'}" \
               f"  [{ev.get('chars',0)}c, {ev.get('destinations',0)} dests]"
    if kind == "reply":
        return f"  reply {turn}: {ev.get('latency_ms',0)}ms" \
               f"{' EMPTY' if ev.get('empty') else ''}"
    if kind == "command":
        return f"{who} -> {ev.get('type')} {json.dumps(ev.get('params', {}))} {turn}"
    if kind == "outcome":
        mark = "ok" if ev.get("ok") else "FAILED"
        detail = f" -- {ev['detail']}" if ev.get("detail") else ""
        stuck = "  <-- STUCK, repeating" if ev.get("stuck") else ""
        return f"{who} {ev.get('type')}: {mark}{detail}{stuck} {turn}"
    if kind == "takeover":
        return (f"{who} taken over  noncombat={ev.get('noncombat')} "
                f"combat={len(ev.get('combat', []))} strategies")
    if kind in ("session", "session_end"):
        return f"--- {kind} {json.dumps({k: v for k, v in ev.items() if k not in ('t','kind','_file')})}"
    return f"{kind} {json.dumps({k: v for k, v in ev.items() if not k.startswith('_')})}"


def cmd_sessions(args):
    paths = sorted(glob.glob(os.path.join(args.dir, "*.jsonl")))
    if not paths:
        sys.exit(f"no telemetry in {args.dir}")

    by_stamp = collections.defaultdict(list)
    for p in paths:
        by_stamp[os.path.basename(p).split("-", 1)[1].replace(".jsonl", "")].append(p)

    for stamp in sorted(by_stamp):
        total = sum(sum(1 for _ in open(p)) for p in by_stamp[stamp])
        size = sum(os.path.getsize(p) for p in by_stamp[stamp])
        print(f"{stamp}  {total:>7} events  {size/1e6:>6.1f} MB  "
              f"{', '.join(os.path.basename(p).split('-')[0] for p in by_stamp[stamp])}")


def cmd_timeline(args):
    events = load(args.dir, args.session)
    for ev in events:
        if args.bot and ev.get("bot") != args.bot:
            continue
        if args.player and ev.get("player") != args.player:
            continue
        if args.kind and ev.get("kind") != args.kind:
            continue
        print(f"{clock(ev)}  {describe(ev)}")


def cmd_turn(args):
    events = load(args.dir, args.session)
    hits = [e for e in events if str(e.get("turn")) == str(args.turn)]
    if not hits:
        sys.exit(f"no events for turn {args.turn}")
    for ev in hits:
        print(f"{clock(ev)}  {describe(ev)}")
        if ev.get("kind") == "prompt" and ev.get("prompt"):
            print("  --- full prompt ---")
            print("  " + ev["prompt"].replace("\n", "\n  "))


def cmd_failures(args):
    events = load(args.dir, args.session)
    counter = collections.Counter()
    examples = {}

    for ev in events:
        if ev.get("kind") == "outcome" and not ev.get("ok"):
            key = (ev.get("bot"), ev.get("type"), ev.get("detail", ""))
            counter[key] += 1
            examples.setdefault(key, ev)
        elif ev.get("kind") == "intent" and not ev.get("executed"):
            key = (ev.get("bot"), f"intent:{ev.get('command')}", ev.get("refused", ""))
            counter[key] += 1
            examples.setdefault(key, ev)

    if not counter:
        print("no failures recorded")
        return

    print(f"{'count':>6}  bot / action / detail")
    for (bot, what, detail), n in counter.most_common(args.limit):
        ev = examples[(bot, what, detail)]
        print(f"{n:>6}  {bot or '-'} {what}  {detail}")
        print(f"{'':>6}  first at {clock(ev)}"
              + (f", turn #{ev['turn']}" if ev.get("turn") else ""))


def cmd_stuck(args):
    """Bots that kept issuing something that kept failing -- the shape of a
    blocking failure, which is what standing still and retrying looks like."""
    events = load(args.dir, args.session)
    runs = collections.defaultdict(int)
    worst = {}

    for ev in events:
        if ev.get("kind") != "outcome":
            continue
        key = (ev.get("bot"), ev.get("type"))
        if ev.get("ok"):
            runs[key] = 0
        else:
            runs[key] += 1
            if runs[key] >= worst.get(key, (0, None))[0]:
                worst[key] = (runs[key], ev)

    rows = [(n, bot, what, ev) for (bot, what), (n, ev) in worst.items() if n >= args.min]
    if not rows:
        print(f"nothing repeated {args.min}+ times")
        return

    for n, bot, what, ev in sorted(rows, reverse=True, key=lambda r: r[0]):
        print(f"{n:>4} consecutive failures  {bot} {what}  "
              f"last at {clock(ev)}  {ev.get('detail','')}")


def cmd_around(args):
    events = load(args.dir, args.session)
    if not events:
        return

    day = dt.datetime.fromtimestamp(events[0]["t"] / 1000).date()
    try:
        hh, mm, ss = (int(x) for x in args.time.split(":"))
    except ValueError:
        sys.exit("time should look like 14:32:05")

    centre = dt.datetime.combine(day, dt.time(hh, mm, ss)).timestamp() * 1000
    lo, hi = centre - args.window * 1000, centre + args.window * 1000

    for ev in events:
        if lo <= ev.get("t", 0) <= hi:
            print(f"{clock(ev)}  {describe(ev)}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", default=DEFAULT_DIR)
    ap.add_argument("--session", help="timestamp substring, e.g. 20260916-1432")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("sessions").set_defaults(func=cmd_sessions)

    p = sub.add_parser("timeline")
    p.add_argument("--bot")
    p.add_argument("--player")
    p.add_argument("--kind")
    p.set_defaults(func=cmd_timeline)

    p = sub.add_parser("turn")
    p.add_argument("turn")
    p.set_defaults(func=cmd_turn)

    p = sub.add_parser("failures")
    p.add_argument("--limit", type=int, default=25)
    p.set_defaults(func=cmd_failures)

    p = sub.add_parser("stuck")
    p.add_argument("--min", type=int, default=3)
    p.set_defaults(func=cmd_stuck)

    p = sub.add_parser("around")
    p.add_argument("time")
    p.add_argument("--window", type=int, default=30, help="seconds either side")
    p.set_defaults(func=cmd_around)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
