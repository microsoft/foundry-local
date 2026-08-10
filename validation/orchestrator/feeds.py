"""Isolated package-feed configuration for the validation harness.

The RC packages under test live ONLY on the ORT-Nightly ADO feed; their transitive
dependencies (cffi, node-addon-api, Microsoft.ML.OnnxRuntime, Betalgo.Ranul.OpenAI, ...)
live on the public registries. The ORT-Nightly feed is anonymous for its OWN packages, but
its upstream proxy requires authentication to *save* an upstream package on first fetch, so
transitive deps must be resolved from a separate deps source, not the ORT feed.

The harness therefore uses TWO sources per ecosystem:
  * the ORT feed  -> the RC package (and only the RC package)
  * a deps source -> everything else (public registry, or a corporate mirror)

Feed locations/credentials come from environment variables so no secrets are committed.
Set these on each agent before running (see validation/VALIDATION.md):

  RC package source (ORT-Nightly):
    FOUNDRY_VALIDATION_NUGET_FEED     NuGet v3 index URL for the ORT-Nightly feed
    FOUNDRY_VALIDATION_NPM_REGISTRY   npm registry URL for the ORT-Nightly feed
    FOUNDRY_VALIDATION_PIP_INDEX      PyPI-style index URL for the ORT-Nightly feed
    FOUNDRY_VALIDATION_FEED_TOKEN     PAT for the ORT feed (only if it is private)

  Transitive-dependency source (defaults to the public registries; override to a mirror on
  locked-down machines whose egress to the public registries is blocked):
    FOUNDRY_VALIDATION_DEPS_NUGET_FEED   default https://api.nuget.org/v3/index.json
    FOUNDRY_VALIDATION_DEPS_NPM_REGISTRY default https://registry.npmjs.org/
    FOUNDRY_VALIDATION_DEPS_PIP_INDEX    default https://pypi.org/simple

The helpers below emit per-run, throwaway config files inside the run's workspace so the
host's global NuGet/npm/pip config is never mutated.
"""
from __future__ import annotations

import os
from typing import Dict, List, Optional

# The RC packages that must be sourced from the ORT feed (everything else -> deps source).
RC_NUGET_PACKAGES: List[str] = ["Microsoft.AI.Foundry.Local", "Microsoft.AI.Foundry.Local.Runtime"]

_DEPS_DEFAULT = {
    "nuget": "https://api.nuget.org/v3/index.json",
    "npm": "https://registry.npmjs.org/",
    "pip": "https://pypi.org/simple",
}


def feed_env() -> Dict[str, Optional[str]]:
    return {
        "nuget": os.environ.get("FOUNDRY_VALIDATION_NUGET_FEED"),
        "npm": os.environ.get("FOUNDRY_VALIDATION_NPM_REGISTRY"),
        "pip": os.environ.get("FOUNDRY_VALIDATION_PIP_INDEX"),
        "token": os.environ.get("FOUNDRY_VALIDATION_FEED_TOKEN"),
    }


def deps_source(kind: str) -> str:
    """Return the transitive-dependency source URL for `kind` (nuget|npm|pip)."""
    env = {
        "nuget": os.environ.get("FOUNDRY_VALIDATION_DEPS_NUGET_FEED"),
        "npm": os.environ.get("FOUNDRY_VALIDATION_DEPS_NPM_REGISTRY"),
        "pip": os.environ.get("FOUNDRY_VALIDATION_DEPS_PIP_INDEX"),
    }[kind]
    return env or _DEPS_DEFAULT[kind]


def missing_feed(sdk: str) -> Optional[str]:
    """Return the env var name required for `sdk` if it is not configured."""
    env = feed_env()
    need = {
        "cpp": ("nuget", "FOUNDRY_VALIDATION_NUGET_FEED"),
        "cs": ("nuget", "FOUNDRY_VALIDATION_NUGET_FEED"),
        "js": ("npm", "FOUNDRY_VALIDATION_NPM_REGISTRY"),
        "python": ("pip", "FOUNDRY_VALIDATION_PIP_INDEX"),
    }[sdk]
    return None if env[need[0]] else need[1]


def write_nuget_config(workspace: str) -> str:
    """Two-source nuget.config: RC packages from the ORT feed, all deps from the deps source."""
    env = feed_env()
    ort = env["nuget"] or "https://api.nuget.org/v3/index.json"
    deps = deps_source("nuget")
    token = env["token"]
    creds = ""
    if token:
        creds = f"""
  <packageSourceCredentials>
    <ortnightly>
      <add key="Username" value="pat" />
      <add key="ClearTextPassword" value="{token}" />
    </ortnightly>
  </packageSourceCredentials>"""
    rc_patterns = "\n      ".join(f'<package pattern="{p}" />' for p in RC_NUGET_PACKAGES)
    cfg = f"""<?xml version="1.0" encoding="utf-8"?>
<configuration>
  <config>
    <add key="globalPackagesFolder" value="{os.path.join(workspace, 'nuget-cache')}" />
  </config>
  <packageSources>
    <clear />
    <add key="ortnightly" value="{ort}" />
    <add key="deps" value="{deps}" />
  </packageSources>
  <packageSourceMapping>
    <packageSource key="ortnightly">
      {rc_patterns}
    </packageSource>
    <packageSource key="deps">
      <package pattern="*" />
    </packageSource>
  </packageSourceMapping>{creds}
</configuration>
"""
    path = os.path.join(workspace, "nuget.config")
    with open(path, "w", encoding="utf-8") as f:
        f.write(cfg)
    return path


def write_npmrc(workspace: str, registry: Optional[str] = None) -> str:
    """Write an isolated .npmrc. `registry` selects the default registry for this file
    (ORT feed for fetching the RC tarball, or the deps registry for resolving deps)."""
    env = feed_env()
    reg = registry or env["npm"]
    lines = [f"cache={os.path.join(workspace, 'npm-cache')}"]
    if reg:
        lines.append(f"registry={reg}")
        if env["token"] and env["npm"] and reg == env["npm"]:
            host = env["npm"].split("://", 1)[-1].rstrip("/")
            lines.append(f"//{host}/:_authToken={env['token']}")
    path = os.path.join(workspace, ".npmrc")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    return path


def pip_install_args(workspace: str) -> Dict[str, str]:
    """Return isolated pip fragments. `ort_index` fetches the RC wheel (no deps); `deps_index`
    resolves transitive deps. See runners._install_smoke_python for the two-step flow."""
    env = feed_env()
    return {
        "ort_index": env["pip"] or "https://pypi.org/simple",
        "deps_index": deps_source("pip"),
        "cache_dir": os.path.join(workspace, "pip-cache"),
    }
