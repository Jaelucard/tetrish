#!/bin/sh
# tests/integration.sh
#
# Stage 1 (always runs): owned-only correctness -- the sanitized
# libtetrisbrain suite, including the 20k-tick determinism gate.
#
# Stage 2 (conditional): a live join -> start -> move -> state -> leave
# session against a real tetrisd, reusing the teammate `stub_client`
# integration harness (tests/stub_client.c) rather than reimplementing the
# protocol here. Only attempted if the daemon binaries and PKI already
# exist; this script never generates keys itself (`bash
# auth/generate_keys.sh` does that, and auth/ is a read-only path for this
# track) and never treats a missing dependency as a failure.
#
# Usage:  sh tests/integration.sh
# Run from the project root.
#   exit 0  everything that ran passed
#   exit 1  something that ran actually failed
#   exit 2  stage 2 was skipped (a dependency was missing) and stage 1
#           otherwise passed

RC=.tetrishrc
fail=0

kill_by_name() {   # no pkill/ps in the dev container, so walk /proc ourselves
    for p in /proc/[0-9]*; do
        c=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
        case "$c" in *"$1"*) kill "${p#/proc/}" 2>/dev/null || true ;; esac
    done
}

echo "== stage 1: owned-only (libtetrisbrain, incl. 20k determinism) =="
if make -s test-san; then
    echo "  ok   make test-san"
else
    echo "  FAIL make test-san"
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo
    echo "== integration: FAIL (stage 1) =="
    exit 1
fi

echo
echo "== stage 2: live join -> start -> move -> state -> leave (optional) =="
make -s build/tests/stub_client bin/tetrisd bin/tetrislogd >/dev/null 2>&1
if [ ! -x bin/tetrisd ] || [ ! -x bin/tetrislogd ] || [ ! -x build/tests/stub_client ] ||
   [ ! -f auth/server.key ] || [ ! -f auth/server.crt ] || [ ! -f auth/cacsertificate.crt ]; then
    echo "  skip missing daemon binaries and/or PKI"
    echo "       run 'bash auth/generate_keys.sh' for the PKI; if the"
    echo "       binaries themselves would not build, see REQUIRED_FILES.md"
    exit 2
fi

[ -f "$RC" ] || cp sample.tetrishrc "$RC"
mkdir -p var/run var/log
kill_by_name "bin/tetrisd"
kill_by_name "bin/tetrislogd"
sleep 1
rm -f var/log/tetrisd.log
# tetrislogd must start AFTER the log file is removed: it holds the file
# open, so unlinking the path while it runs leaves it writing to a dead
# inode (same ordering constraint as tests/gate8.sh).
./bin/tetrislogd "$RC" >/dev/null 2>&1 &
sleep 1
./bin/tetrisd "$RC" >/dev/null 2>&1 &
sleep 2

OUT=$(./build/tests/stub_client "$RC" integration-smoke 5); rc=$?
echo "$OUT" | sed 's/^/  /'
if [ "$rc" -eq 0 ]; then
    echo "  ok   stub_client (join/start/move/state/leave)"
else
    echo "  FAIL stub_client (join/start/move/state/leave)"
    fail=1
fi

if [ -x bin/tetrisctl ]; then ./bin/tetrisctl "$RC" shutdown >/dev/null 2>&1; sleep 1; fi
kill_by_name "bin/tetrisd"
kill_by_name "bin/tetrislogd"

echo
echo "== integration: $([ "$fail" -eq 0 ] && echo PASS || echo FAIL) =="
exit "$fail"
