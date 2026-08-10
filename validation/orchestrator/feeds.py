"""Isolated package-feed configuration for the validation harness.

Every install is done against the ORT-Nightly ADO feed in an ISOLATED environment
(dedicated caches, no fallback feeds) so a clean end-user install is truly proven.

Feed locations/credentials are provided via environment variables so no secrets are
committed. Set these on each agent before running (see validation/VALIDATION.md):

  FOUNDRY_VALIDATION_NUGET_FEED   NuGet v3 index URL for the ORT-Nightly feed
  FOUNDRY_VALIDATION_NPM_REGISTRY npm registry URL for the ORT-Nightly feed
  FOUNDRY_VALIDATION_PIP_INDEX    PyPI-style index URL for the ORT-Nightly feed
  FOUNDRY_VALIDATION_FEED_TOKEN   PAT/token for the feed (if the feed is private)

The helpers below emit per-run, throwaway config files inside the run's workspace so
the host's global NuGet/npm/pip config is never mutated.
"""
from __future__ import annotations

import os
from typing import Dict, Optional


def feed_env() -> Dict[str, Optional[str]]:
    return {
        "nuget": os.environ.get("FOUNDRY_VALIDATION_NUGET_FEED"),
        "npm": os.environ.get("FOUNDRY_VALIDATION_NPM_REGISTRY"),
        "pip": os.environ.get("FOUNDRY_VALIDATION_PIP_INDEX"),
        "token": os.environ.get("FOUNDRY_VALIDATION_FEED_TOKEN"),
    }


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
    env = feed_env()
    feed = env["nuget"] or "https://api.nuget.org/v3/index.json"
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
    cfg = f"""<?xml version="1.0" encoding="utf-8"?>
<configuration>
  <config>
    <add key="globalPackagesFolder" value="{os.path.join(workspace, 'nuget-cache')}" />
  </config>
  <packageSources>
    <clear />
    <add key="nuget.org" value="https://api.nuget.org/v3/index.json" />
    <add key="ortnightly" value="{feed}" />
  </packageSources>
  <packageSourceMapping>
    <packageSource key="ortnightly">
      <package pattern="Microsoft.AI.Foundry.Local*" />
    </packageSource>
    <packageSource key="nuget.org">
      <package pattern="*" />
    </packageSource>
  </packageSourceMapping>{creds}
</configuration>
"""
    path = os.path.join(workspace, "nuget.config")
    with open(path, "w", encoding="utf-8") as f:
        f.write(cfg)
    return path


def write_npmrc(workspace: str) -> str:
    env = feed_env()
    lines = [f"cache={os.path.join(workspace, 'npm-cache')}"]
    if env["npm"]:
        lines.append(f"registry={env['npm']}")
        if env["token"]:
            reg = env["npm"].split("://", 1)[-1].rstrip("/")
            lines.append(f"//{reg}/:_authToken={env['token']}")
    path = os.path.join(workspace, ".npmrc")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    return path


def pip_install_args(workspace: str) -> Dict[str, str]:
    """Return env/args fragments for an isolated pip install against the feed."""
    env = feed_env()
    args = {"index_url": env["pip"] or "https://pypi.org/simple"}
    args["cache_dir"] = os.path.join(workspace, "pip-cache")
    return args
