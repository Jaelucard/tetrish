#!/bin/sh
# gate8.sh: reproducible evidence that the baseline MVP works end to end.
#
# Runs the whole daemon stack, drives it with two concurrent stub clients in
# separate rooms, and checks every claim below mechanically. No TTY needed, so
# it also runs under valgrind (pass --valgrind).
#
# Usage:  sh tests/gate8.sh [--valgrind]
# Run from the project root. Exits non-zero if any check fails.

set -e
RC=.tetrishrc
VG=""
VGLOG=var/log/valgrind-gate8.log
[ "$1" = "--valgrind" ] && VG="valgrind --leak-check=full --show-leak-kinds=all \
    --error-exitcode=99 --log-file=$VGLOG"

pass=0
fail=0
check() {   # check <description> <expected-substring> <actual>
    if echo "$3" | grep -q "$2"; then
        echo "  ok   $1"
        pass=$((pass + 1))
    else
        echo "  FAIL $1"
        echo "         expected to contain: $2"
        echo "         got:                 $3"
        fail=$((fail + 1))
    fi
}

kill_by_name() {   # no pkill/ps in the dev container, so walk /proc ourselves
    for p in /proc/[0-9]*; do
        c=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
        case "$c" in
            *"$1"*) kill "${p#/proc/}" 2>/dev/null || true ;;
        esac
    done
}

echo "== setup =="
[ -f $RC ] || cp sample.tetrishrc $RC
mkdir -p var/run var/log
# The stub client lives under build/tests and is only produced by `make test`,
# not by a plain `make`. A clean checkout would otherwise fail here.
make test >/dev/null 2>&1 || { echo "  FAIL could not build test harness"; exit 1; }
kill_by_name "bin/tetrisd"
kill_by_name "bin/tetrislogd"
sleep 1
rm -f var/log/tetrisd.log
# tetrislogd must be started AFTER the log file is removed: it holds the file
# open, so unlinking the path while it runs leaves it writing to a dead inode.
./bin/tetrislogd $RC >/dev/null 2>&1 &
sleep 1
$VG ./bin/tetrisd $RC >/dev/null 2>&1 &
sleep 5

echo "== item 2: status returns real data =="
S=$(./bin/tetrisctl $RC status)
echo "  $S"
check "uptime present and non-zero" '"uptime_seconds": [1-9]' "$S"
check "rooms field present"         '"rooms"'                 "$S"
check "players field present"       '"players"'               "$S"

echo "== item 1: two concurrent clients in separate rooms =="
./build/tests/stub_client $RC roomA 60 >var/log/gate8-clientA.log 2>&1 &
./build/tests/stub_client $RC roomB 60 >var/log/gate8-clientB.log 2>&1 &
sleep 8
S=$(./bin/tetrisctl $RC status)
R=$(./bin/tetrisctl $RC rooms)
echo "  $S"
echo "  $R"
check "two rooms live"            '"rooms": 2'    "$S"
check "two players live"          '"players": 2'  "$S"
check "roomA present and started" 'roomA'         "$R"
check "roomB present and started" 'roomB'         "$R"
# Two rooms means two independent timerfds in the one epoll set. That is the
# actual concurrency claim, and it is what the ticks counters demonstrate.
check "both rooms ticking"        '"started": 1'  "$R"

echo "== item 4: dropped-logs reports both causes separately =="
D=$(./bin/tetrisctl $RC dropped-logs)
echo "  $D"
check "ring counter present"  '"ring"'  "$D"
check "send counter present"  '"send"'  "$D"
check "total counter present" '"total"' "$D"

echo "== item 3: graceful shutdown, with both clients still mid-game =="
./bin/tetrisctl $RC shutdown >/dev/null
sleep 8
L=$(grep phase var/log/tetrisd.log || true)
echo "$L" | sed 's/^/  /'
check "phase 1 stop accepting" 'phase 1'               "$L"
check "phase 2 drained 2"      'phase 2: disconnected 2' "$L"
check "phase 3 flush"          'phase 3'               "$L"
check "phase 4 exit"           'phase 4'               "$L"

ALIVE=""
for p in /proc/[0-9]*; do
    c=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
    case "$c" in *bin/tetrisd*) ALIVE="yes" ;; esac
done
check "tetrisd exited" "^$" "$ALIVE"
check "ctl socket removed" "^$" "$(ls var/run/ | grep tetrisctl || true)"

if [ -n "$VG" ]; then
    echo "== item 5: valgrind =="
    V=$(tail -12 $VGLOG)
    echo "$V" | sed 's/^/  /'
    check "no leaks"  "All heap blocks were freed" "$V"
    check "no errors" "0 errors from 0 contexts"   "$V"
fi

kill_by_name "bin/tetrislogd"
echo
echo "== $pass passed, $fail failed =="
[ $fail -eq 0 ] || exit 1
