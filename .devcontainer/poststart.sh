#!/usr/bin/env bash
# Runs every time the container starts

# tetriSH has nothing that needs re-running on each start yet, so this is a
# near no-op, kept for symmetry and future use (e.g. starting the daemons
# automatically once they exist).
set -euo pipefail
echo "[poststart] container started."
