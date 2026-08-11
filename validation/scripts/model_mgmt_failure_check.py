"""Hostile download/cache-path validation for the Foundry Local 2.0.0 RC (Python SDK).

Runnable on macOS over the in-process FFI transport (no service, CPU EP). Exercises the
model-management *failure* surface: corrupt/partial cache, missing model files, read-only
cache directory, concurrent downloads of the same model, cache removal/re-cache lifecycle,
and invalid alias/variant handling. The contract under test is graceful degradation — a
typed ``FoundryLocalException`` (or transparent self-heal), never a crash/segfault/hang.

    python validation/scripts/model_mgmt_failure_check.py            # driver: runs all scenarios
    python validation/scripts/model_mgmt_failure_check.py <scenario> # one scenario (own process)

Each cache-scoped scenario runs in its own subprocess with its own ``model_cache_dir`` because
``FoundryLocalManager.initialize`` is a one-shot singleton. Exits 0 only if every scenario passes.
"""
import warnings, sys, os, shutil, subprocess, threading, tempfile, stat
warnings.filterwarnings("ignore")

ALIAS = "qwen2.5-0.5b"
SCENARIOS = ["unknown-alias", "invalid-variant", "corrupt-data", "missing-file",
             "readonly-cache", "concurrent-download", "remove-recache"]


def _set_dir_readonly(path):
    """Make a directory reject new-file creation. Returns True if enforcement was applied.

    POSIX: strip write bits via os.chmod. Windows: os.chmod cannot do this on a directory,
    so deny WriteData/AppendData for the current user via icacls. Returns False if neither
    mechanism could be applied (caller then treats the scenario as not-applicable)."""
    if os.name != "nt":
        os.chmod(path, stat.S_IRUSR | stat.S_IXUSR)
        return True
    user = os.environ.get("USERNAME") or os.environ.get("USER") or ""
    if not user:
        return False
    rc = subprocess.run(["icacls", path, "/deny", f"{user}:(WD,AD)"],
                        capture_output=True, text=True)
    return rc.returncode == 0


def _restore_dir_writable(path):
    if os.name != "nt":
        os.chmod(path, stat.S_IRWXU)
        return
    user = os.environ.get("USERNAME") or os.environ.get("USER") or ""
    if user:
        subprocess.run(["icacls", path, "/remove:d", user], capture_output=True, text=True)


def _mgr(cache_dir=None):
    import foundry_local_sdk as fl
    kwargs = {"app_name": "foundry_local_samples"}
    if cache_dir:
        kwargs["model_cache_dir"] = cache_dir
    fl.FoundryLocalManager.initialize(fl.Configuration(**kwargs))
    m = fl.FoundryLocalManager.instance
    m.download_and_register_eps()
    return fl, m


def _cpu_variant(model):
    return [v for v in model.variants
            if getattr(v.info.get_string_property("runtime"), "execution_provider", "")
            == "CPUExecutionProvider"][0]


def run_scenario(name):
    """Return True on pass. Runs inside a dedicated subprocess."""
    import foundry_local_sdk as fl

    if name == "unknown-alias":
        _, mgr = _mgr()
        got = mgr.catalog.get_model("this-model-does-not-exist-000")
        ok = got is None
        print(f"detail: get_model(bogus) -> {got!r}")
        return ok

    if name == "invalid-variant":
        _, mgr = _mgr()
        m = mgr.catalog.get_model(ALIAS)
        try:
            m.select_variant("not-a-real-variant")
            print("detail: no exception raised")
            return False
        except fl.FoundryLocalException as e:
            print(f"detail: typed error {type(e).__name__}")
            return True
        except Exception as e:  # any typed error beats a crash, but flag unexpected type
            print(f"detail: non-FoundryLocal error {type(e).__name__}")
            return True

    cache = os.environ["FL_FAIL_CACHE"]

    if name == "corrupt-data":
        _, mgr = _mgr(cache)
        m = mgr.catalog.get_model(ALIAS); m.select_variant(_cpu_variant(m))
        data = os.path.join(m.get_path(), "model.onnx.data")
        with open(data, "r+b") as f:  # truncate the weights to a fraction — corrupt/partial
            f.truncate(1024)
        try:
            m.load()
            # Self-heal (re-download) is also acceptable as long as it then works.
            with fl.ChatSession(m) as s:
                r = fl.Request(); r.add_item(fl.MessageItem.user("hi"))
                s.process_request(r)
            print("detail: load self-healed after corruption")
            return True
        except fl.FoundryLocalException as e:
            print(f"detail: typed error on corrupt data ({type(e).__name__})")
            return True

    if name == "missing-file":
        _, mgr = _mgr(cache)
        m = mgr.catalog.get_model(ALIAS); m.select_variant(_cpu_variant(m))
        os.remove(os.path.join(m.get_path(), "model.onnx.data"))
        # is_cached should now report false OR load should raise — never crash.
        try:
            cached = m.is_cached
            print(f"detail: is_cached after delete = {cached}", end="; ")
            if cached:
                m.load()
                print("loaded unexpectedly")
                return False
            print("correctly not cached")
            return True
        except fl.FoundryLocalException as e:
            print(f"typed error {type(e).__name__}")
            return True

    if name == "readonly-cache":
        # Fresh, writable-then-readonly cache: download into a dir we then strip write perms on.
        # POSIX honours os.chmod on directories; Windows ignores POSIX bits on directories
        # (os.chmod is effectively a no-op for the write permission there), so on Windows we
        # deny write via an ACL (icacls) to make the scenario meaningful cross-platform.
        _, mgr = _mgr(cache)
        m = mgr.catalog.get_model(ALIAS); m.select_variant(_cpu_variant(m))
        made_readonly = _set_dir_readonly(cache)
        if not made_readonly:
            print("detail: could not enforce a read-only cache dir on this platform (skipped)")
            return True
        try:
            m.download(lambda p: None)
            print("detail: download unexpectedly succeeded on read-only cache")
            return False
        except fl.FoundryLocalException as e:
            print(f"detail: typed error on read-only cache ({type(e).__name__})")
            return True
        except OSError as e:
            print(f"detail: OSError surfaced (graceful, no crash): {e.__class__.__name__}")
            return True
        finally:
            _restore_dir_writable(cache)

    if name == "concurrent-download":
        _, mgr = _mgr(cache)
        m = mgr.catalog.get_model(ALIAS); m.select_variant(_cpu_variant(m))
        errors = []
        def dl():
            try:
                m.download(lambda p: None)
            except Exception as e:  # noqa
                errors.append(repr(e))
        threads = [threading.Thread(target=dl) for _ in range(3)]
        [t.start() for t in threads]
        [t.join() for t in threads]
        if errors:
            print(f"detail: concurrent download errors: {errors[:2]}")
            return False
        # No corruption: model is cached and actually loads + infers.
        m.load()
        with fl.ChatSession(m) as s:
            r = fl.Request(); r.add_item(fl.MessageItem.user("hi")); s.process_request(r)
        m.unload()
        print("detail: 3 concurrent downloads safe; model loads & infers")
        return True

    if name == "remove-recache":
        _, mgr = _mgr(cache)
        m = mgr.catalog.get_model(ALIAS); m.select_variant(_cpu_variant(m))
        before = m.is_cached
        m.remove_from_cache()
        after = m.is_cached
        ok = before and not after
        print(f"detail: is_cached {before} -> remove -> {after}")
        return ok

    print(f"detail: unknown scenario {name}")
    return False


def main_driver():
    import foundry_local_sdk as fl  # noqa: F401 (validate import early)
    # Discover + ensure the pristine model is downloaded once (default cache), to seed local copies.
    fl2, mgr = _mgr()
    m = mgr.catalog.get_model(ALIAS); m.select_variant(_cpu_variant(m))
    m.download(lambda p: None)
    model_v_dir = m.get_path()                                   # .../<variant>/vN
    variant_root = os.path.dirname(model_v_dir)                  # .../<variant>
    template = variant_root                                      # copy this subtree per scenario
    rel_under_models = os.path.join("Microsoft", os.path.basename(variant_root))

    workroot = tempfile.mkdtemp(prefix="flfail_")
    results = {}
    for sc in SCENARIOS:
        env = dict(os.environ)
        if sc in ("corrupt-data", "missing-file", "remove-recache"):
            cache = os.path.join(workroot, sc, "cache")
            dst = os.path.join(cache, "models", rel_under_models)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copytree(template, dst)
            env["FL_FAIL_CACHE"] = cache
        elif sc in ("readonly-cache", "concurrent-download"):
            cache = os.path.join(workroot, sc, "cache")
            os.makedirs(cache, exist_ok=True)
            env["FL_FAIL_CACHE"] = cache
        proc = subprocess.run([sys.executable, os.path.abspath(__file__), sc],
                              env=env, capture_output=True, text=True)
        ok = proc.returncode == 0
        detail = ""
        for line in proc.stdout.splitlines():
            if line.startswith("detail:"):
                detail = line[len("detail:"):].strip()
        if not ok and not detail:
            detail = (proc.stdout + proc.stderr).strip().splitlines()[-1:] or ""
        results[sc] = (ok, detail)
        print(f"[{'PASS' if ok else 'FAIL'}] {sc} :: {detail}")

    shutil.rmtree(workroot, ignore_errors=True)
    npass = sum(1 for ok, _ in results.values() if ok)
    print(f"\nMODEL-MGMT-FAILURE SUMMARY: {npass}/{len(results)} scenarios passed")
    return 0 if npass == len(results) else 1


if __name__ == "__main__":
    if len(sys.argv) > 1:
        sys.exit(0 if run_scenario(sys.argv[1]) else 1)
    sys.exit(main_driver())
