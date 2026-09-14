#!/usr/bin/env python3
"""Capture real bot prompts by sitting between the module and Ollama.

Why this exists: prompt changes measured against hand-written prompts have been
wrong three times in this project (see docs/PROMPT-EVALS.md). Real prompts are
~24,000 characters and instructions behave completely differently when buried in
one. Always measure against captured prompts.

Usage:
    CAP=/tmp/cap.jsonl python3 tools/capture_prompts.py &
    # point the module at it, no rebuild needed:
    #   OllamaBotControl.Url = http://host.docker.internal:11435/api/generate
    #   OllamaBotControl.RequirePlayerOnline = 0
    #   OllamaBotControl.TestBotCount = 3
    # restart worldserver, wait for rows, then PUT THE CONFIG BACK.

Leaving RequirePlayerOnline = 0 pins a 4.3GB model and has caused OOM kills.
"""
import http.server, socketserver, urllib.request, json, time, os, threading
UP="http://127.0.0.1:11434"; OUT=os.environ.get("CAP","/tmp/cap.jsonl")
lock=threading.Lock()
class H(http.server.BaseHTTPRequestHandler):
    protocol_version="HTTP/1.1"
    def log_message(self,*a): pass
    def do_POST(self):
        n=int(self.headers.get("Content-Length",0)); body=self.rfile.read(n)
        t0=time.time()
        try:
            req=urllib.request.Request(UP+self.path,data=body,
                headers={"Content-Type":"application/json"},method="POST")
            resp=urllib.request.urlopen(req,timeout=600); data=resp.read(); code=resp.status
        except Exception as e:
            data=json.dumps({"error":str(e)}).encode(); code=500
        dt=time.time()-t0
        rec={"ms":round(dt*1000)}
        try:
            j=json.loads(body); p=j.get("prompt","")
            rec.update({"prompt_chars":len(p),"model":j.get("model"),
                        "has_format":bool(j.get("format")),
                        "format_chars":len(json.dumps(j.get("format"))) if j.get("format") else 0,
                        "prompt":p})
        except Exception: pass
        try:
            d=json.loads(data)
            for k in ("prompt_eval_count","eval_count","total_duration",
                      "prompt_eval_duration","eval_duration","load_duration"):
                if k in d: rec[k]=d[k]
            rec["response"]=d.get("response","")[:400]
        except Exception: pass
        with lock:
            with open(OUT,"a") as f: f.write(json.dumps(rec)+"\n")
        self.send_response(code); self.send_header("Content-Type","application/json")
        self.send_header("Content-Length",str(len(data))); self.end_headers()
        self.wfile.write(data)
class S(socketserver.ThreadingTCPServer): allow_reuse_address=True; daemon_threads=True
S(("0.0.0.0",11435),H).serve_forever()
