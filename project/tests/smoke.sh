#!/bin/sh
# tests/smoke.sh -- headless, non-interactive smoke test for the owned
# tree (libtetrisbrain, tetrisu, tetrish-view). No ncurses touched, no
# daemon started; see tests/integration.sh for the daemon-dependent path.
#
# Two tiers, reported separately, because the shared Makefile builds every
# track's binaries together: a break in a teammate path must not hide
# whether the code THIS track owns is actually fine.
#
#   1. Owned-only (`make test-san`): the sanitized libtetrisbrain/
#      tetrish-view test suite. Builds and runs with no dependency on
#      libtetrissh or libhtttp at all -- this is what decides pass/fail.
#   2. Full tree (`make` then `make test`): informational. Its failure is
#      reported but does not by itself fail this script. If it fails,
#      check REQUIRED_FILES.md before assuming this track broke it.
#
# Usage:  sh tests/smoke.sh [logfile]
# Run from the project root.
#   exit 0  tier 1 passed (tier 2 may still have failed; see the output)
#   exit 1  tier 1 actually failed
#   exit 2  tier 1 passed but the optional --verify step was skipped
#           (no logfile argument and no var/log/tetrisd.log to fall back to)

fail=0
check() {   # check <description> <status>
    if [ "$2" -eq 0 ]; then echo "  ok   $1"; else echo "  FAIL $1"; fail=1; fi
}

echo "== smoke tier 1: owned code, no teammate libraries required =="
make -s test-san
check "make test-san (sanitized libtetrisbrain suite, incl. 20k determinism)" $?

echo
echo "== smoke tier 2: full tree (informational) =="
if make -s >/dev/null 2>&1 && make -s test >/dev/null 2>&1; then
    echo "  ok   make && make test"
else
    echo "  note full-tree build/test did not pass -- see REQUIRED_FILES.md"
    echo "       for whether this is a known teammate-side gap before"
    echo "       treating it as a regression in owned code"
fi

echo
echo "== smoke: tetrish-view --verify (optional) =="
LOG="${1:-var/log/tetrisd.log}"
if [ ! -f "$LOG" ]; then
    echo "  skip no replay log at $LOG (pass one as \$1 to check this)"
    [ "$fail" -eq 0 ] && exit 2
else
    if [ -x bin/tetrish-view ]; then
        OUT=$(bin/tetrish-view --verify "$LOG" 2>&1); rc=$?
        echo "$OUT" | sed 's/^/  /'
        check "tetrish-view --verify $LOG" "$rc"
    else
        echo "  skip bin/tetrish-view was not built (see tier 2 above)"
        [ "$fail" -eq 0 ] && exit 2
    fi
fi

echo
echo "== smoke: $([ "$fail" -eq 0 ] && echo PASS || echo FAIL) =="
exit "$fail"
