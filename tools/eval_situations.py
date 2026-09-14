#!/usr/bin/env python3
"""Replay captured bot prompts, injecting one situation at a time.

Measures whether an instruction is actually followed when buried in a real
~24,000 character prompt, and compares placing the situation block at the end of
the prompt versus the top. See docs/PROMPT-EVALS.md.

Usage:
    python3 tools/eval_situations.py CAPTURE.jsonl [CAPTURE2.jsonl ...] \
        [--bases N] [--samples N]

CAPTURE files are produced by tools/capture_prompts.py.
"""
import json,re,urllib.request,sys
from collections import Counter

args=[a for a in sys.argv[1:] if not a.startswith("--")]
opts={a.split("=")[0]:a.split("=")[1] for a in sys.argv[1:] if a.startswith("--") and "=" in a}
if not args:
    print(__doc__); sys.exit(2)
recs=[]
for f in args:
    try:
        recs+=[json.loads(l) for l in open(f)]
    except Exception as e:
        print(f"could not read {f}: {e}"); sys.exit(2)
base=[r["prompt"] for r in recs if r.get("prompt")]
if not base:
    print("no prompts found in the capture files"); sys.exit(2)
MODEL=opts.get("--model","qwen2.5:7b-instruct")
URL=opts.get("--url","http://127.0.0.1:11434/api/generate")
SITBLK=r'(?m)^SITUATION:.*(?:\n(?!\n).*)*\n?'
def strip_sit(p): return re.sub(SITBLK,'',p)
def guids(p): return sorted({int(x) for x in re.findall(r'\(guid:?\s*(\d+)',p)}) or [1]
def ndest(p): return len(re.findall(r'^\s*\[(\d+)\]',p,re.M)) or 1
# faithful situation lines, copied from BuildSituationAssessment
def sit_low_def(p):  return "SITUATION: You are at 14% health and in combat. You will die if you keep attacking. Cast Ice Block (spell 45438) now, or move away to disengage. Do not attack this turn.\n"
def sit_low_nodef(p):return "SITUATION: You are at 14% health and in combat with no defensive ability ready. Move away to disengage rather than trading more damage.\n"
def sit_loot(p):     return f"SITUATION: There is a lootable corpse within reach (guid {guids(p)[0]}, 3 yards). Loot it now. Do not move, and do not attack anything else first.\n"
def sit_turnin(p):   return f"SITUATION: Marshal Dughan (guid {guids(p)[0]}) has your completed quest and is within reach at 2 yards. Turn the quest in now. You do not need to move.\n"
CASES={
 "low HP + defensive": (sit_low_def,  {"spell","move_to"}, True),
 "low HP no defensive":(sit_low_nodef,{"move_to"},         True),
 "lootable corpse":    (sit_loot,     {"loot"},            False),
 "quest turn-in":      (sit_turnin,   {"turn_in_quest","interact"}, False),
}
def set_combat(p):
    return re.sub(r'^(NOT IN COMBAT|IN COMBAT).*$',
                  'IN COMBAT (MELEE FIGHTER). Your HP: 52/372, Mana: 786/786, Energy: 100/100',
                  p,count=1,flags=re.M)
def mk(n,pr,rq): return {"type":"object","properties":{"type":{"type":"string","enum":[n]},
    "params":{"type":"object","properties":pr,"required":rq}},"required":["type","params"]}
def schema_for(p):
    g=guids(p); nd=ndest(p)
    v=[mk("move_to",{"destination_index":{"type":"integer","enum":list(range(nd))}},["destination_index"]),
       mk("attack",{"attack_guid":{"type":"integer","enum":g}},["attack_guid"]),
       mk("interact",{"guid":{"type":"integer","enum":g}},["guid"]),
       mk("spell",{"spell_id":{"type":"integer"}},["spell_id"])]
    for s in ("loot","follow","stop"): v.append(mk(s,{},[]))
    for s in ("accept_quest","turn_in_quest"): v.append(mk(s,{"quest_id":{"type":"integer"}},["quest_id"]))
    return {"type":"object","properties":{"steps":{"type":"array","minItems":1,"maxItems":4,
            "items":{"oneOf":v}}},"required":["steps"]}
def call(p,sch):
    b={"model":MODEL,"prompt":p,"stream":False,"format":sch,"keep_alive":"10m",
       "options":{"num_predict":200,"temperature":0.2}}
    d=json.load(urllib.request.urlopen(urllib.request.Request(
        URL,data=json.dumps(b).encode(),
        headers={"Content-Type":"application/json"}),timeout=600))
    return json.loads(d["response"])
NB=int(opts.get("--bases",4))
NS=int(opts.get("--samples",3))
bases=base[:NB]
print(f"base prompts: {len(bases)}  (median {sorted(len(b) for b in bases)[len(bases)//2]} chars)  samples each: {NS}\n")
print(f"{'situation':<22}{'END (shipped)':>16}{'TOP (old)':>14}   notes")
for name,(mkline,good,combat) in CASES.items():
    row={}
    for pos in ("end","top"):
        ok=0;n=0;c=Counter()
        for bp in bases:
            p=strip_sit(bp)
            if combat: p=set_combat(p)
            line=mkline(bp)
            p = (p.rstrip()+"\n\n"+line) if pos=="end" else (line+"\n"+p)
            sch=schema_for(bp)
            for _ in range(NS):
                try: st=call(p,sch).get("steps",[])
                except Exception: continue
                if not st: continue
                t=st[0].get("type"); c[t]+=1; n+=1
                if t in good: ok+=1
        row[pos]=(ok,n,c)
    e=row["end"]; t=row["top"]
    top3=", ".join(f"{k}:{v}" for k,v in e[2].most_common(3))
    print(f"{name:<22}{e[0]:>8}/{e[1]:<7}{t[0]:>7}/{t[1]:<6}   end-> {top3}")
