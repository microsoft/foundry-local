"""Direct model-management validation for the Foundry Local 2.0.0 RC (Python SDK).

There is no per-SDK model-management *sample* for Python/JS/C++, so this script exercises the
model-management runtime surface directly against the RC API over the in-process FFI transport
(no service required). Run it inside a venv that has `foundry-local-sdk==2.0.0rc1` installed:

    python validation/scripts/model_mgmt_check.py

Exits 0 only if every check passes. Covers: catalog list, get_model, CPU+WebGPU variant
enumeration, explicit variant selection, download + idempotent cache-reuse, get_cached_models,
load/unload lifecycle, and latest-version query.
"""
import warnings, sys, time
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

# 1. Catalog list
models = cat.list_models()
check("catalog.list_models returns many models", len(models) > 20, f"{len(models)} models")

# 2. get_model by alias
m = cat.get_model("qwen2.5-0.5b")
check("get_model('qwen2.5-0.5b')", m is not None and m.alias == "qwen2.5-0.5b", m.alias)

# 3. variants + EP metadata (CPU + WebGPU present)
eps = set()
for v in m.variants:
    rt = v.info.get_string_property("runtime")
    eps.add(getattr(rt, "execution_provider", str(rt)))
check("model exposes CPU + WebGPU variants", {"CPUExecutionProvider", "WebGPUExecutionProvider"} <= eps, str(sorted(eps)))

# 4. Explicit variant selection (CPU)
cpu = [v for v in m.variants if getattr(v.info.get_string_property("runtime"), "execution_provider", "") == "CPUExecutionProvider"][0]
m.select_variant(cpu)
check("select_variant(CPU) succeeds", True, cpu.id)

# 5. Download + cache reuse (idempotent, is_cached flips true)
mgr.download_and_register_eps()
m.download(lambda p: None)
check("model.is_cached after download", m.is_cached, "cached")
t0 = time.time()
m.download(lambda p: None)  # second download should be a fast no-op (cache reuse)
reuse_s = time.time() - t0
check("second download is cache-reuse (fast)", reuse_s < 5.0, f"{reuse_s:.2f}s")

# 6. get_cached_models includes our model
try:
    cached = cat.get_cached_models()
    check("get_cached_models lists cached entries", len(cached) >= 1, f"{len(cached)} cached")
except Exception as e:
    check("get_cached_models lists cached entries", False, repr(e))

# 7. load -> is_loaded -> unload lifecycle
m.load()
check("model.is_loaded after load", m.is_loaded, "loaded")
m.unload()
check("model.is_loaded False after unload", not m.is_loaded, "unloaded")

# 8. get_latest_version metadata query
try:
    lv = cat.get_latest_version(m)
    check("get_latest_version returns a value", lv is not None, str(lv))
except Exception as e:
    check("get_latest_version returns a value", False, repr(e))

npass = sum(1 for _, ok, _ in results if ok)
print(f"\nMODEL-MGMT SUMMARY: {npass}/{len(results)} checks passed")
sys.exit(0 if npass == len(results) else 1)
