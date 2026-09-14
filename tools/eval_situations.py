import json,re,urllib.request,sys
from collections import Counter
B="/private/tmp/claude-501/-Users-justinc-code/340c6ab3-0f23-48e9-9305-1d102224742c/scratchpad/"
recs=[]
for f in ("cap4.jsonl","cap3.jsonl"):
    try: recs+=[json.loads(l) for l in open(B+f)]
    except Exception: pass
base=[r["prompt"] for r in recs if r.get("prompt")]
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
    b={"model":"qwen2.5:7b-instruct","prompt":p,"stream":False,"format":sch,"keep_alive":"10m",
       "options":{"num_predict":200,"temperature":0.2}}
    d=json.load(urllib.request.urlopen(urllib.request.Request(
        "http://127.0.0.1:11434/api/generate",data=json.dumps(b).encode(),
        headers={"Content-Type":"application/json"}),timeout=600))
    return json.loads(d["response"])
NB=int(sys.argv[1]) if len(sys.argv)>1 else 4
NS=int(sys.argv[2]) if len(sys.argv)>2 else 3
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
