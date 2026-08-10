"""Soak & resource-gate validation for the Foundry Local 2.0.0 RC (Python SDK).

Runnable entirely on macOS over the in-process FFI transport (no service, CPU EP):
repeated load/unload, long-running streaming, a cancellation race, concurrent
inference across sessions, RSS/handle-growth measurement, and coarse
startup/latency regression gates.

    python validation/scripts/soak_resource_check.py

Exits 0 only if every gate passes. Thresholds are deliberately coarse — they catch
gross regressions (leaks, wedges, multi-minute startups), not micro-latency drift.
"""
import warnings, sys, os, time, threading, subprocess
warnings.filterwarnings("ignore")
import foundry_local_sdk as fl

PID = os.getpid()
results = []
def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print(f"[{'PASS' if ok else 'FAIL'}] {name} :: {detail}")

def rss_mb():
    out = subprocess.check_output(["ps", "-o", "rss=", "-p", str(PID)]).strip()
    return int(out) / 1024.0  # macOS ps reports KB

def ask(session, text):
    req = fl.Request(); req.add_item(fl.MessageItem.user(text))
    resp = session.process_request(req)
    parts = [resp.get_item(i).get_simple_text() for i in range(resp.item_count)
             if isinstance(resp.get_item(i), fl.MessageItem)]
    return "".join(parts)

cfg = fl.Configuration(app_name="foundry_local_samples")
fl.FoundryLocalManager.initialize(cfg)
mgr = fl.FoundryLocalManager.instance
mgr.download_and_register_eps()
m = mgr.catalog.get_model("qwen2.5-0.5b")
cpu = [v for v in m.variants
       if getattr(v.info.get_string_property("runtime"), "execution_provider", "") == "CPUExecutionProvider"][0]
m.select_variant(cpu)
m.download(lambda p: None)

# GATE 1: repeated load/unload (soak) — every cycle loads, infers, and unloads cleanly, and
# the steady-state RSS slope must be small (i.e. no unbounded per-model leak). A true leak of
# the model would add ~model-size (hundreds of MB) every cycle; allocator/mmap retention instead
# plateaus. We measure the slope over the *second half* of cycles to ignore warm-up growth.
N_CYCLES = 16
load_time0 = None
ok_cycles = True
unload_rss = []
for i in range(N_CYCLES):
    t0 = time.time()
    m.load()
    if i == 0:
        load_time0 = time.time() - t0
    with fl.ChatSession(m) as s:
        r = ask(s, "Reply with OK.")
    m.unload()
    unload_rss.append(rss_mb())
    if not r.strip():
        ok_cycles = False
half = N_CYCLES // 2
steady_slope = (unload_rss[-1] - unload_rss[half]) / (N_CYCLES - half)  # MB per cycle, steady state
check(f"{N_CYCLES} load/unload cycles all infer & unload cleanly", ok_cycles, "")
check("load/unload steady-state RSS slope < 25MB/cycle (no runaway leak)", steady_slope < 25.0,
      f"warmup {unload_rss[0]:.0f}->{unload_rss[half]:.0f}MB, steady slope {steady_slope:.1f}MB/cyc, "
      f"plateau {unload_rss[-1]:.0f}MB")

# Load once for the remaining gates.
m.load()

# GATE 2: long-running streaming — many chunks, first-token latency captured, final response present.
sstream = fl.ChatSession(m); sstream.set_streaming(True)
req = fl.Request(); req.add_item(fl.MessageItem.user("List the numbers 1 through 40, one per line."))
t0 = time.time(); first_token_dt = None; nchunks = 0; text = []
sr = sstream.process_streaming_request(req)
for chunk in sr:
    if first_token_dt is None:
        first_token_dt = time.time() - t0
    nchunks += 1
    if isinstance(chunk, fl.TextItem):
        text.append(chunk.text)
final = sr.final_response
check("long streaming yields many chunks + final response", nchunks > 20 and final is not None,
      f"{nchunks} chunks, final={type(final).__name__}")
sstream._close()

# GATE 3: cancellation race — cancel a stream mid-flight; worker winds down, process stays healthy.
scan = fl.ChatSession(m); scan.set_streaming(True)
creq = fl.Request(); creq.add_item(fl.MessageItem.user("Write a very long, detailed 2000-word essay about the ocean."))
seen = {"n": 0}
def consume():
    try:
        for _ in scan.process_streaming_request(creq):
            seen["n"] += 1
            if seen["n"] >= 3:
                creq.cancel()
    except Exception:
        pass
th = threading.Thread(target=consume); th.start()
th.join(timeout=30)
cancel_clean = (not th.is_alive())
scan._close()
# Process still healthy after a mid-stream cancel?
post = ask(fl.ChatSession(m), "Reply with ALIVE.") if not th.is_alive() else ""
check("mid-stream cancel winds down without wedging", cancel_clean and bool(post.strip()),
      f"consumed>={seen['n']}, post-cancel reply={post[:20]!r}")

# GATE 4: concurrent inference — several sessions issue requests simultaneously; all respond.
outs = {}
def worker(idx):
    with fl.ChatSession(m) as s:
        outs[idx] = ask(s, f"Reply with THREAD{idx}.")
threads = [threading.Thread(target=worker, args=(i,)) for i in range(4)]
[t.start() for t in threads]
[t.join(timeout=60) for t in threads]
all_responded = len(outs) == 4 and all(v.strip() for v in outs.values())
check("4 concurrent sessions all respond", all_responded, f"{len(outs)}/4 responded")

# GATE 5: memory/handle growth over sustained inference on one session.
soak_sess = fl.ChatSession(m)
rss_start = rss_mb()
for _ in range(20):
    ask(soak_sess, "Say hi.")
rss_end = rss_mb()
growth = rss_end - rss_start
check("20-inference soak RSS growth bounded (<150MB)", growth < 150,
      f"{rss_start:.0f}->{rss_end:.0f}MB (+{growth:.0f})")
soak_sess._close()

# GATE 6: coarse startup/latency regression gates.
check("model load time under 60s", load_time0 is not None and load_time0 < 60,
      f"{load_time0:.1f}s" if load_time0 else "n/a")
check("first-token latency under 30s", first_token_dt is not None and first_token_dt < 30,
      f"{first_token_dt:.2f}s" if first_token_dt else "n/a")

m.unload()
npass = sum(1 for _, ok, _ in results if ok)
print(f"\nSOAK/RESOURCE SUMMARY: {npass}/{len(results)} gates passed")
sys.exit(0 if npass == len(results) else 1)
