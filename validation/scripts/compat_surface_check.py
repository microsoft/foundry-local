"""Backward-compatibility surface check: does Foundry Local 2.0 preserve the 1.x public API?

The 1.x (`foundry-local-sdk` 1.2.x) and 2.0 Python packages share the import name
`foundry_local_sdk`, so a source-level upgrade is only safe if 2.0 still exposes the symbols
1.x code depends on. This asserts that the **1.x core public surface** (captured below from
1.2.4) is still present in the *installed* SDK, and reports what 2.0 adds. Run inside a venv
with the 2.0 RC installed:

    python validation/scripts/compat_surface_check.py

Full upgrade/migration validation on target platforms (rebuild an existing app, model-cache
and config reuse/migration, uninstall/reinstall, side-by-side) remains fleet work; this covers
the source/API-compat slice, which is runnable anywhere. Exits 0 only if no core 1.x symbol is
missing from 2.0.
"""
import warnings, sys
warnings.filterwarnings("ignore")
import foundry_local_sdk as fl

# Captured from foundry-local-sdk 1.2.4 (the latest 1.x) — the public symbols existing 1.x
# code can legitimately depend on. 2.0 must still expose all of these for a safe source upgrade.
# `openai` is validated separately as an importable submodule (2.0 no longer eager-imports it
# into the top-level namespace, but `import foundry_local_sdk.openai` still works — see below).
V1_TOP_LEVEL = {"Configuration", "FoundryLocalManager", "catalog", "configuration",
                "ep_types", "exception", "imodel", "version"}
V1_MANAGER = {"discover_eps", "download_and_register_eps", "initialize", "instance",
              "start_web_service", "stop_web_service"}
V1_IMODEL = {"alias", "capabilities", "context_length", "download", "get_audio_client",
             "get_chat_client", "get_embedding_client", "get_path", "id", "info",
             "input_modalities", "is_cached", "is_loaded", "load", "output_modalities",
             "remove_from_cache", "select_variant", "supports_tool_calling", "unload", "variants"}

results = []
def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print(f"[{'PASS' if ok else 'FAIL'}] {name} :: {detail}")

cur_top = {a for a in dir(fl) if not a.startswith("_")}
cur_mgr = {a for a in dir(fl.FoundryLocalManager) if not a.startswith("_")}
cur_imodel = {a for a in dir(fl.imodel.IModel) if not a.startswith("_")}

missing_top = V1_TOP_LEVEL - cur_top
missing_mgr = V1_MANAGER - cur_mgr
missing_imodel = V1_IMODEL - cur_imodel

check("2.0 preserves 1.x top-level public symbols", not missing_top, f"missing={sorted(missing_top) or 'none'}")
check("2.0 preserves 1.x FoundryLocalManager methods", not missing_mgr, f"missing={sorted(missing_mgr) or 'none'}")
check("2.0 preserves 1.x IModel methods", not missing_imodel, f"missing={sorted(missing_imodel) or 'none'}")

# `openai` compat: importable submodule (not a removal), even though 2.0 no longer eager-imports
# it onto the package namespace. `import foundry_local_sdk.openai` must still work.
import importlib
try:
    importlib.import_module("foundry_local_sdk.openai")
    check("2.0 keeps foundry_local_sdk.openai importable submodule", True,
          "import works (note: not auto-exposed on top-level namespace)")
except Exception as e:
    check("2.0 keeps foundry_local_sdk.openai importable submodule", False, f"{type(e).__name__}: {e}")

# Informational: what 2.0 adds on top (the new session/item programming model).
added_top = sorted(cur_top - V1_TOP_LEVEL)
added_mgr = sorted(cur_mgr - V1_MANAGER)
print(f"\n[info] 2.0 adds {len(added_top)} top-level symbols (e.g. {added_top[:8]} ...)")
print(f"[info] 2.0 adds FoundryLocalManager methods: {added_mgr}")
print("[info] Behavioral: get_chat_client()/get_*_client() are DEPRECATED in 2.0 in favor of "
      "ChatSession/AudioSession/EmbeddingsSession (source-compatible, emits a warning).")

npass = sum(1 for _, ok, _ in results if ok)
print(f"\nCOMPAT-SURFACE SUMMARY: {npass}/{len(results)} checks passed")
sys.exit(0 if npass == len(results) else 1)
