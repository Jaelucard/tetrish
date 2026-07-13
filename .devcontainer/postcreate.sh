#!/usr/bin/env bash
# Runs once inside the container, right after it is first created.
set -euo pipefail

# The repo is bind-mounted from the host, so its files are owned by the host
# user, not by root inside the container. Without this, git refuses to operate
# ("detected dubious ownership in repository"). Mark the repo safe.
git config --global --add safe.directory /work

# Sanity-check that the toolchain issue 1 requires is actually present.
echo "[postcreate] toolchain versions:"
gcc --version | head -1
make --version | head -1
valgrind --version
openssl version
echo "[postcreate] environment ready -- run 'make' in /work/project to build."
