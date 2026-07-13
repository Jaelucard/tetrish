#!/usr/bin/env bash
# Runs on the host before container is built

# tetriSH needs almost nothing here. We only make sure the runtime dirs the
# daemons write to exist before they get bind-mounted in, so a fresh checkout
# never trips over a missing var/run or var/log.
set -euo pipefail
mkdir -p project/var/run project/var/log
echo "[initialize] host prep done."