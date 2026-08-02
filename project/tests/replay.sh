#!/bin/sh
# replay.sh: record a real session, then prove it can be reconstructed.
#
# This is the test that makes the replay claim mean something. It runs the
# actual daemons, plays a real game, fires garbage at it, then asks
# tetrish-view to rebuild the session from nothing but tetrislogd's log file
# and compares the reconstruction against the SNAPSHOT records the server
# wrote as it went.
#
# The comparison is the point. Snapshots are not used to seek (they cannot be:
# a snapshot restores the board but not the piece randomiser, so jumping to one
# would invent the pieces that follow). They are checkpoints, and a mismatch
# means a record never reached the log.
#
# Usage: sh tests/replay.sh
# Run from the project root. Exits non-zero if a reconstruction does not match.

set -e
RC=.tetrishrc
pass=0
fail=0

check() {
    if [ "$2" = "$3" ]; then
        echo "  ok   $1  ($3)"; pass=$((pass + 1))
    else
        echo "  FAIL $1"; echo "         expected: $2"; echo "         actual:   $3"
        fail=$((fail + 1))
    fi
}
kill_by_name() {
    for p in /proc/[0-9]*; do
        c=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
        case "$c" in *"$1"*) kill "${p#/proc/}" 2>/dev/null || true ;; esac
    done
}

echo "== setup =="
[ -f $RC ] || cp sample.tetrishrc $RC
mkdir -p var/run var/log
make test >/dev/null 2>&1 || { echo "  FAIL build"; exit 1; }
kill_by_name "bin/tetrisd"
kill_by_name "bin/tetrislogd"
sleep 1
# Remove the log BEFORE starting tetrislogd. It holds the file open, so
# unlinking the path under a running logger leaves it writing to a dead inode.
rm -f var/log/tetrisd.log
./bin/tetrislogd $RC >/dev/null 2>&1 &
sleep 1
./bin/tetrisd $RC >/dev/null 2>&1 &
sleep 2

echo "== record a session: two rooms, one garbage hit =="
./build/tests/stub_client $RC repA 16 >/dev/null 2>&1 &
./build/tests/stub_client $RC repB 16 >/dev/null 2>&1 &
sleep 5
./build/tests/garbage_send $RC repA repB 3 5 >/dev/null
sleep 13
./bin/tetrisctl $RC shutdown >/dev/null
sleep 3
kill_by_name "bin/tetrislogd"
sleep 1

echo "== the log actually contains replay records =="
check "SEED records"     "2" "$(grep -c ' E .* SEED '    var/log/tetrisd.log || true)"
check "GARBAGE records"  "1" "$(grep -c ' E .* GARBAGE ' var/log/tetrisd.log || true)"
SNAPS=$(grep -c ' S ' var/log/tetrisd.log || true)
echo "  snapshot records: $SNAPS"
[ "$SNAPS" -gt 0 ] && { echo "  ok   snapshots were written"; pass=$((pass + 1)); } \
                   || { echo "  FAIL no snapshots"; fail=$((fail + 1)); }

echo "== reconstruct each session from the log alone =="
for room in repA repB; do
    echo "  --- $room ---"
    if ./bin/tetrish-view --verify var/log/tetrisd.log "$room" > var/log/replay-$room.txt 2>&1; then
        sed 's/^/    /' var/log/replay-$room.txt
        echo "  ok   $room reconstruction matches every recorded snapshot"
        pass=$((pass + 1))
    else
        sed 's/^/    /' var/log/replay-$room.txt
        echo "  FAIL $room reconstruction diverged from the server's snapshots"
        fail=$((fail + 1))
    fi
done

echo "== a log with no session is rejected, not silently replayed =="
: > var/log/empty.log
if ./bin/tetrish-view --verify var/log/empty.log >/dev/null 2>&1; then
    echo "  FAIL an empty log should not verify"; fail=$((fail + 1))
else
    echo "  ok   an empty log is refused"; pass=$((pass + 1))
fi

echo
echo "== $pass passed, $fail failed =="
[ $fail -eq 0 ] || exit 1
