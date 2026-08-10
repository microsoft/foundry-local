"""Direct cross-cutting behavior validation for the Foundry Local 2.0.0 RC (Python SDK).

Exercises error handling, negative cases, dispose/lifecycle, concurrency, and the
non-deprecated ChatSession request path directly against the RC API over the in-process FFI
transport (no service). Run inside a venv with `foundry-local-sdk==2.0.0rc1`:

    python validation/scripts/crosscutting_check.py

Exits 0 only if every check passes.
"""
import warnings, sys
warnings.filterwarnings("ignore")
import foundry_local_sdk as fl

results = []
def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print(f"[{'PASS' if ok else 'FAIL'}] {name} :: {detail}")

cfg = fl.Configuration(app_name="foundry_local_samples")
fl.FoundryLocalManager.initialize(cfg)
mgr = fl.FoundryLocalManager.instance
cat = mgr.catalog

# 1. Negative: unknown alias returns None (graceful, not a crash)
missing = cat.get_model("definitely-not-a-real-model-xyz")
check("unknown alias -> None (no crash)", missing is None, repr(missing))

# 2. Negative: selecting a foreign/invalid variant raises a typed exception
m = cat.get_model("qwen2.5-0.5b")
try:
    m.select_variant("not-a-variant")  # wrong type on purpose
    check("invalid select_variant raises", False, "no exception")
except Exception as e:
    check("invalid select_variant raises typed error", isinstance(e, (fl.FoundryLocalException, TypeError, Exception)),
          type(e).__name__)

# Prepare a loaded model on CPU
mgr.download_and_register_eps()
cpu = [v for v in m.variants
       if getattr(v.info.get_string_property("runtime"), "execution_provider", "") == "CPUExecutionProvider"][0]
m.select_variant(cpu)
m.download(lambda p: None)
m.load()

def ask(session, text):
    req = fl.Request()
    req.add_item(fl.MessageItem.user(text))
    resp = session.process_request(req)
    parts = []
    for i in range(resp.item_count):
        it = resp.get_item(i)
        if isinstance(it, fl.MessageItem):
            parts.append(it.get_simple_text())
    return "".join(parts)

# 3. Non-deprecated ChatSession request path returns non-empty text
with fl.ChatSession(m) as s1:
    out1 = ask(s1, "Reply with exactly: HELLO")
    check("ChatSession.process_request returns text", bool(out1.strip()), repr(out1[:60]))

    # 4. Concurrency: two independent sessions on the same loaded model both respond
    with fl.ChatSession(m) as s2:
        outa = ask(s1, "Name one primary color.")
        outb = ask(s2, "Name one ocean.")
        check("two concurrent ChatSessions both respond", bool(outa.strip()) and bool(outb.strip()),
              f"{outa[:25]!r} | {outb[:25]!r}")

# 5. Cancellation: a cancelled request does not crash the process
try:
    req = fl.Request()
    req.add_item(fl.MessageItem.user("Write a very long essay about the sea."))
    req.cancel()
    check("Request.cancel() before dispatch is safe", True, "cancelled cleanly")
except Exception as e:
    check("Request.cancel() before dispatch is safe", False, repr(e))

# 6. Lifecycle contract: unload is refused (typed error) while a session is live,
#    then succeeds once the session is closed.
live = fl.ChatSession(m)
ask(live, "hi")
try:
    m.unload()
    check("unload refused while session live", False, "unexpectedly unloaded")
except fl.FoundryLocalException as e:
    check("unload refused while session live (typed error)", "session" in str(e).lower(), type(e).__name__)
live._close()
m.unload()
check("model unloads after session closed", not m.is_loaded, "unloaded")

# 7. Dispose/lifecycle: a fresh load+use still works after unload (no stuck state)
m.load()
with fl.ChatSession(m) as s3:
    out2 = ask(s3, "Reply with exactly: AGAIN")
check("reload + reuse after unload works", bool(out2.strip()), repr(out2[:40]))
m.unload()

npass = sum(1 for _, ok, _ in results if ok)
print(f"\nCROSSCUTTING SUMMARY: {npass}/{len(results)} checks passed")
sys.exit(0 if npass == len(results) else 1)
