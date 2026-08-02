#!/bin/sh
# week10.sh - Battle Royale under load, per-IP limit, and room/timerfd cleanup.
#
# Three things are checked, and the third is the one that needs measuring
# rather than asserting:
#
#   1. Many concurrent rooms tick together and garbage flows between them,
#      with every event accounted for (applied + discarded == sent).
#   2. The per-IP connection limit refuses past the cap, logs at warn level,
#      and counts the refusal.
#   3. Room cleanup returns the daemon to its baseline file descriptor count.
#      Each started room owns a timerfd; if room_leave's orphan handoff ever
#      broke, tetrisd would leak one fd per finished room and nothing else
#      would notice until it ran out.
#
# Usage: sh tests/week10.sh
# Run from the project root. Exits non-zero if any check fails.

set -e
RC=.tetrishrc
pass=0
fail=0

check() {   # check <description> <expected> <actual>
    if [ "$2" = "$3" ]; then
        echo "  ok   $1  ($3)"
        pass=$((pass + 1))
    else
        echo "  FAIL $1"
        echo "         expected: $2"
        echo "         actual:   $3"
        fail=$((fail + 1))
    fi
}
check_contains() {
    if echo "$3" | grep -q "$2"; then
        echo "  ok   $1"
        pass=$((pass + 1))
    else
        echo "  FAIL $1 (wanted '$2' in: $3)"
        fail=$((fail + 1))
    fi
}

# No pkill or ps in the dev container, so walk /proc.
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

echo "== setup =="
[ -f $RC ] || cp sample.tetrishrc $RC
mkdir -p var/run var/log
make test >/dev/null 2>&1 || { echo "  FAIL build"; exit 1; }
kill_by_name "bin/tetrisd"
kill_by_name "bin/tetrislogd"
sleep 1
rm -f var/log/tetrisd.log
./bin/tetrislogd $RC >/dev/null 2>&1 &
sleep 1

# ---------------------------------------------------------------------------
echo "== 1. per-IP connection limit =="
# Run this first, with the configured cap in force, before we relax it for the
# load test below.
CAP=$(grep -E "^max_conns_per_ip" $RC | awk '{print $2}')
CAP=${CAP:-8}
./bin/tetrisd $RC >/dev/null 2>&1 &
sleep 2
OVER=$((CAP + 4))
i=1
while [ $i -le $OVER ]; do
    ./build/tests/stub_client $RC iplimit$i 15 >/dev/null 2>&1 &
    i=$((i + 1))
done
sleep 6
S=$(./bin/tetrisctl $RC status)
check "accepted exactly the cap"      "\"players\": $CAP"  "$(echo "$S" | grep -o "\"players\": [0-9]*")"
check "refused the rest"              "\"rejected_conns\": 4" "$(echo "$S" | grep -o '"rejected_conns": [0-9]*')"
check "refusals logged at warn level" "4" "$(grep -c 'refused: already at the per-IP limit' var/log/tetrisd.log)"
./bin/tetrisctl $RC shutdown >/dev/null
sleep 4

# ---------------------------------------------------------------------------
echo "== 2. Battle Royale under load, many concurrent rooms =="
# The per-IP cap has to come off here: every connection in this test genuinely
# originates from 127.0.0.1, which is exactly the case the 0 setting exists for.
sed 's/^max_conns_per_ip.*/max_conns_per_ip 0/' $RC > var/run/loadtest.rc
grep -q "max_conns_per_ip" var/run/loadtest.rc || echo "max_conns_per_ip 0" >> var/run/loadtest.rc

# tetrislogd must be restarted around this rm, not just have the file removed
# under it. It holds the log open, so unlinking the path leaves it writing to a
# dead inode and the path simply never reappears.
kill_by_name "bin/tetrislogd"
sleep 1
rm -f var/log/tetrisd.log
./bin/tetrislogd $RC >/dev/null 2>&1 &
sleep 1
./bin/tetrisd var/run/loadtest.rc >/dev/null 2>&1 &
sleep 2
PID=$(find_pid "bin/tetrisd")
BASE_FD=$(fd_count "$PID")
echo "  baseline fds with no rooms: $BASE_FD"

ROOMS=10
i=1
while [ $i -le $ROOMS ]; do
    ./build/tests/stub_client var/run/loadtest.rc load$i 22 >/dev/null 2>&1 &
    i=$((i + 1))
done
sleep 8

S=$(./bin/tetrisctl var/run/loadtest.rc status)
R=$(./bin/tetrisctl var/run/loadtest.rc rooms)
check "all rooms live concurrently" "\"rooms\": $ROOMS" "$(echo "$S" | grep -o '"rooms": [0-9]*')"
check_contains "every room is ticking" '"started": 1' "$R"

LOAD_FD=$(fd_count "$PID")
# Two fds per room here, not one: the client's TCP socket, plus the timerfd
# that START created for that room. Both have to come back at the end.
echo "  fds with $ROOMS rooms running:  $LOAD_FD  ($ROOMS client sockets + $ROOMS timerfds)"
check "each room costs one socket and one timerfd" "$((BASE_FD + ROOMS * 2))" "$LOAD_FD"

# Fire garbage at a steady, realistic rate: one event per room, spaced out.
# "No lost events at normal rates" means the 10-deep queue never fills when
# events arrive slower than the epoll loop drains them.
echo "  firing $ROOMS garbage events at a normal rate..."
i=1
while [ $i -le $ROOMS ]; do
    dst=$(( (i % ROOMS) + 1 ))
    ./build/tests/garbage_send var/run/loadtest.rc load$i load$dst 2 $((i % 10)) >/dev/null
    sleep 0.2
    i=$((i + 1))
done
sleep 2

APPLIED=$(grep -c "garbage applied" var/log/tetrisd.log || true)
MISSED=$(grep -c "is gone, event discarded" var/log/tetrisd.log || true)
DROPPED=$(grep -c "garbage DROPPED" var/log/tetrisd.log || true)
echo "  applied=$APPLIED discarded=$MISSED dropped=$DROPPED of $ROOMS sent"
check "every event was accounted for" "$ROOMS" "$((APPLIED + MISSED + DROPPED))"
check "nothing was lost to a full queue at normal rates" "0" "$DROPPED"

# ---------------------------------------------------------------------------
echo "== 3. room cleanup returns fds to baseline =="
# Let every client finish and leave of its own accord, then confirm the daemon
# is back where it started. This is the check that would catch a leaked timerfd.
sleep 18
S=$(./bin/tetrisctl var/run/loadtest.rc status)
check "all rooms destroyed"   '"rooms": 0'   "$(echo "$S" | grep -o '"rooms": [0-9]*')"
check "all players gone"      '"players": 0' "$(echo "$S" | grep -o '"players": [0-9]*')"

END_FD=$(fd_count "$PID")
echo "  fds after every room finished: $END_FD  (baseline was $BASE_FD)"
check "no file descriptors leaked" "$BASE_FD" "$END_FD"

# The fd count above is the authoritative proof that every room was cleaned up.
# The log is NOT authoritative, and this is deliberate: ring_push uses trylock
# and drops the record if the logshipper holds the lock, so that gameplay never
# waits on logging. Under this load (10 rooms ticking, 10 clients leaving in the
# same instant) some records genuinely do get dropped, and the drop counter is
# what accounts for them. A test that demanded exactly $ROOMS log lines would be
# asserting a property the architecture explicitly does not promise.
DESTROYED=$(grep -c 'room destroyed' var/log/tetrisd.log || true)
DROPS=$(./bin/tetrisctl var/run/loadtest.rc dropped-logs)
echo "  destroy records that reached the log: $DESTROYED of $ROOMS"
echo "  drop accounting: $DROPS"
RING=$(echo "$DROPS" | grep -o '"ring": [0-9]*' | grep -o '[0-9]*')
SEND=$(echo "$DROPS" | grep -o '"send": [0-9]*' | grep -o '[0-9]*')
TOTAL=$(echo "$DROPS" | grep -o '"total": [0-9]*' | grep -o '[0-9]*')
check "drop counters add up" "$TOTAL" "$((RING + SEND))"
# ring drops would mean the game path could not hand a record over, i.e. tetrisd
# is outrunning its own shipper. That is the counter that must stay at zero:
# it is the one gameplay can feel.
check "game path never blocked on logging (ring drops)" "0" "$RING"
if [ "$DESTROYED" -lt "$ROOMS" ]; then
    echo "  note: $((ROOMS - DESTROYED)) destroy records never reached the log."
    echo "        ring=0 means the game path always handed them over, so the loss"
    echo "        is at the send stage: an AF_UNIX datagram queue holds only"
    echo "        net.unix.max_dgram_qlen messages (10 here), and ten rooms"
    echo "        disconnecting at once bursts past that. By design, and counted."
    echo "        Cleanup itself is proven by the fd count above, not by the log."
fi

./bin/tetrisctl var/run/loadtest.rc shutdown >/dev/null
sleep 3
kill_by_name "bin/tetrislogd"
rm -f var/run/loadtest.rc

echo
echo "== $pass passed, $fail failed =="
[ $fail -eq 0 ] || exit 1
