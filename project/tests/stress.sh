#!/bin/sh
# stress.sh: 20+ concurrent clients against one tetrisd.
#
# The clients are spread across several MULTI-PLAYER rooms rather than one room
# each. That is the harder case and the more realistic one: every tick, a room
# has to advance N games, render N boards into one STATE frame, and then
# encrypt that frame separately for each of its players. Twenty solo rooms
# would exercise the epoll set but not the per-room work.
#
# The room and player counts are chosen to stay inside the configured caps
# (max_rooms 16, max_players_per_room 6 by default), because the point is to
# stress the server, not to watch it correctly refuse connections. The per-IP
# limit is turned off for the same reason: every client here genuinely comes
# from 127.0.0.1, which is the case that setting exists for.
#
# Usage: sh tests/stress.sh [clients] [seconds]
# Run from the project root. Exits non-zero if anything fails.

set -e
RC=.tetrishrc
CLIENTS=${1:-20}
SECONDS_RUN=${2:-20}
ROOMS=4
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
check_atleast() {
    if [ "$3" -ge "$2" ] 2>/dev/null; then
        echo "  ok   $1  ($3 >= $2)"; pass=$((pass + 1))
    else
        echo "  FAIL $1 ($3 < $2)"; fail=$((fail + 1))
    fi
}
find_pid() {
    for p in /proc/[0-9]*; do
        c=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
        case "$c" in *"$1"*) echo "${p#/proc/}"; return ;; esac
    done
}
kill_by_name() {
    for p in /proc/[0-9]*; do
        c=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
        case "$c" in *"$1"*) kill "${p#/proc/}" 2>/dev/null || true ;; esac
    done
}
fd_count() { ls /proc/"$1"/fd 2>/dev/null | wc -l; }

echo "== setup: $CLIENTS clients across $ROOMS rooms for ${SECONDS_RUN}s =="
[ -f $RC ] || cp sample.tetrishrc $RC
mkdir -p var/run var/log
make test >/dev/null 2>&1 || { echo "  FAIL build"; exit 1; }
kill_by_name "bin/tetrisd"
kill_by_name "bin/tetrislogd"
sleep 1

sed 's/^max_conns_per_ip.*/max_conns_per_ip 0/' $RC > var/run/stress.rc
grep -q "max_conns_per_ip" var/run/stress.rc || echo "max_conns_per_ip 0" >> var/run/stress.rc

# Remove the log before starting the logger; it holds the file open, so
# unlinking under a running logger leaves it writing to a dead inode.
rm -f var/log/tetrisd.log
./bin/tetrislogd $RC >/dev/null 2>&1 &
sleep 1
./bin/tetrisd var/run/stress.rc >/dev/null 2>&1 &
sleep 2

PID=$(find_pid "bin/tetrisd")
BASE_FD=$(fd_count "$PID")
echo "  tetrisd pid $PID, baseline fds $BASE_FD"

echo "== launching $CLIENTS clients =="
i=1
while [ $i -le $CLIENTS ]; do
    room="s$(( (i % ROOMS) + 1 ))"
    ./build/tests/stub_client var/run/stress.rc "$room" "$SECONDS_RUN" \
        > var/log/stress-$i.log 2>&1 &
    i=$((i + 1))
done
sleep 8

echo "== mid-flight measurements =="
S=$(./bin/tetrisctl var/run/stress.rc status)
R=$(./bin/tetrisctl var/run/stress.rc rooms)
echo "  $S"
check "all clients connected" "\"players\": $CLIENTS" "$(echo "$S" | grep -o '"players\": [0-9]*' || echo "$S" | grep -o '"players": [0-9]*')"
check "rooms in use"          "\"rooms\": $ROOMS"     "$(echo "$S" | grep -o '"rooms": [0-9]*')"

LOAD_FD=$(fd_count "$PID")
# One socket per client, plus one timerfd per started room.
check "fds account for every client and room" \
      "$((BASE_FD + CLIENTS + ROOMS))" "$LOAD_FD"

# The control plane must stay answerable while the game plane is saturated.
# That is the whole reason it lives on a separate AF_UNIX socket rather than
# sharing the TCP listener: a flood on one cannot starve the other.
CTL_OK=0
n=1
while [ $n -le 5 ]; do
    ./bin/tetrisctl var/run/stress.rc status >/dev/null 2>&1 && CTL_OK=$((CTL_OK + 1))
    n=$((n + 1))
done
check "control plane still answers under load" "5" "$CTL_OK"

echo "== fire garbage between rooms while loaded =="
i=1
while [ $i -le $ROOMS ]; do
    dst=$(( (i % ROOMS) + 1 ))
    ./build/tests/garbage_send var/run/stress.rc "s$i" "s$dst" 2 $((i % 10)) >/dev/null || true
    i=$((i + 1))
done
sleep 2
APPLIED=$(grep -c "garbage applied" var/log/tetrisd.log || true)
check_atleast "garbage was applied under load" 1 "$APPLIED"

echo "== waiting for clients to finish =="
sleep $((SECONDS_RUN - 6))
sleep 4

PASSED=$(grep -l "ALL PASS" var/log/stress-*.log 2>/dev/null | wc -l)
check "every client completed its protocol run" "$CLIENTS" "$PASSED"

# Each client reports the STATE rate it measured. They should all be near
# tick_hz; a server that fell behind under load would show up here first.
echo "  observed STATE rates:"
grep -h "STATE stream" var/log/stress-*.log 2>/dev/null | head -3 | sed 's/^/    /'
SLOW=$(grep -h "STATE stream" var/log/stress-*.log 2>/dev/null \
       | grep -c "1[0-5]\.[0-9] Hz" || true)
check "no client saw a badly degraded tick rate" "0" "$SLOW"

echo "== after everyone left =="
S=$(./bin/tetrisctl var/run/stress.rc status)
echo "  $S"
check "all rooms torn down"   '"rooms": 0'   "$(echo "$S" | grep -o '"rooms": [0-9]*')"
check "all players gone"      '"players": 0' "$(echo "$S" | grep -o '"players": [0-9]*')"
END_FD=$(fd_count "$PID")
check "no descriptors leaked" "$BASE_FD" "$END_FD"

D=$(./bin/tetrisctl var/run/stress.rc dropped-logs)
echo "  drop accounting: $D"
RING=$(echo "$D" | grep -o '"ring": [0-9]*' | grep -o '[0-9]*')
# On ring drops, and why this is deliberately not asserted to be zero.
#
# ring_push takes the ring lock with trylock and drops the record if it cannot
# have it. That IS the design: the game path must never wait on logging. So a
# non-zero ring counter is the mechanism working, and demanding exactly zero
# would be asserting that contention never occurs rather than that the design
# holds.
#
# This check did demand zero, and passed, back when the log carried only prose
# records. Adding the replay log (a SEED, every INPUT, every GARBAGE, and a full
# SNAPSHOT per player every snapshot_interval ticks) multiplied the volume, so
# occasional collisions with the shipper's drain are now normal and expected.
#
# What is worth asserting is that they stay RARE. A handful per run is the
# design absorbing a burst. A large number would mean the shipper cannot keep
# up, and the replay log would then have real holes in it.
echo "  ring drops: $RING  (nonzero is expected; trylock drops so the game never waits)"
if [ "$RING" -le 20 ] 2>/dev/null; then
    echo "  ok   ring drops stayed rare ($RING of a limit of 20)"
    pass=$((pass + 1))
else
    echo "  FAIL ring drops are no longer rare ($RING > 20): the shipper is falling behind"
    fail=$((fail + 1))
fi

./bin/tetrisctl var/run/stress.rc shutdown >/dev/null
sleep 3
kill_by_name "bin/tetrislogd"
rm -f var/run/stress.rc var/log/stress-*.log

echo
echo "== $pass passed, $fail failed =="
[ $fail -eq 0 ] || exit 1
