#!/usr/bin/env bash
# Foundry Local 2.0.0 validation runner — Linux/macOS wrapper.
# Bootstraps prerequisites and invokes the stdlib-only orchestrator.
# Pass any orchestrator flags through, e.g.:  ./run.sh --list  |  ./run.sh --sdk python
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ORCH="$SCRIPT_DIR/orchestrator/run_validation.py"

PY="${PYTHON:-python3}"
if ! command -v "$PY" >/dev/null 2>&1; then
  echo "ERROR: python3 not found. Install Python 3.10+ and retry." >&2
  exit 1
fi

# Non-fatal reminder about feed configuration (real runs need these).
for v in FOUNDRY_VALIDATION_NUGET_FEED FOUNDRY_VALIDATION_NPM_REGISTRY FOUNDRY_VALIDATION_PIP_INDEX; do
  if [ -z "${!v:-}" ]; then
    echo "note: \$$v is not set — cells needing that feed will be reported as 'blocked'." >&2
  fi
done

exec "$PY" "$ORCH" "$@"
