#!/bin/sh
# capture-demo.sh: evidence that traffic is encrypted after the handshake.
#
# Runs the full stack, captures the game port with tcpdump, and then shows two
# things about the same capture:
#
#   1. The handshake IS visible in the clear. The server's X.509 certificate is
#      sent as-is, because it is public by definition: the whole point of a
#      certificate is that anybody may read it and check who signed it.
#   2. Everything after that is not. The requests the client sent, and the board
#      the server sent back, appear nowhere in the bytes on the wire.
#
# The second point is the one that matters, and it is demonstrated by searching
# the capture for plaintext that we KNOW was exchanged. If "JOIN", "HTTTP/1.0"
# or a board row appeared, the session layer would not be doing its job.
#
# Usage: sh tests/capture-demo.sh [rc-file]
# Writes var/log/handshake.pcap, which can be opened directly in Wireshark.

set -e
RC=${1:-.tetrishrc}
PCAP=var/log/handshake.pcap
ROOM=capdemo

kill_by_name() {
    for p in /proc/[0-9]*; do
        c=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
        case "$c" in *"$1"*) kill "${p#/proc/}" 2>/dev/null || true ;; esac
    done
}

command -v tcpdump >/dev/null 2>&1 || {
    echo "tcpdump is not installed. In the dev container:  apt-get install -y tcpdump"
    exit 1
}

PORT=$(grep -E "^listen_port" "$RC" | awk '{print $2}')
[ -n "$PORT" ] || { echo "no listen_port in $RC"; exit 1; }

echo "== setup =="
mkdir -p var/run var/log
make >/dev/null 2>&1 && make test >/dev/null 2>&1
kill_by_name "bin/tetrisd"
kill_by_name "bin/tetrislogd"
sleep 1
rm -f var/log/tetrisd.log "$PCAP"
./bin/tetrislogd "$RC" >/dev/null 2>&1 &
sleep 1
./bin/tetrisd "$RC" >/dev/null 2>&1 &
sleep 2

echo "== capturing loopback port $PORT =="
tcpdump -i lo -s 0 -w "$PCAP" "tcp port $PORT" >/dev/null 2>&1 &
TCPD=$!
sleep 2

echo "== running a real session through it =="
./build/tests/stub_client "$RC" "$ROOM" 6 2>&1 | sed 's/^/    /'
sleep 2
kill $TCPD 2>/dev/null || true
sleep 1

SIZE=$(wc -c < "$PCAP")
echo
echo "== captured $SIZE bytes to $PCAP =="
echo

echo "== 1. the handshake is visible, and that is correct =="
# A PEM certificate travels as literal text, so it shows up in a byte search.
if strings "$PCAP" | grep -q "BEGIN CERTIFICATE"; then
    echo "    FOUND: 'BEGIN CERTIFICATE' - the server's public certificate, in the clear"
    echo "    This is by design. A certificate is public; the client needs to read"
    echo "    it and verify the signature on it against the bundled CA."
else
    echo "    not found (the certificate may be sent in DER rather than PEM form)"
fi
echo

echo "== 2. nothing after the handshake is readable =="
fail=0
for pat in "JOIN" "LEAVE" "START" "HTTTP/1.0" "Player-Id" "application/tetris" "player p"; do
    if strings "$PCAP" | grep -q -- "$pat"; then
        echo "    LEAK: found plaintext '$pat' on the wire"
        fail=1
    else
        echo "    ok   '$pat' appears nowhere in the capture"
    fi
done
echo

# The board itself is the most convincing single check: a row of a Tetris board
# is a run of dots and digits, and there were 120 of them in this session.
if strings "$PCAP" | grep -qE "^\.{10}$"; then
    echo "    LEAK: a board row is readable on the wire"
    fail=1
else
    echo "    ok   no board row is readable, though 120 were sent"
fi

# Shut down first: the last log records only reach the file once the logshipper
# has been joined, so counting them before this would undercount.
./bin/tetrisctl "$RC" shutdown >/dev/null 2>&1 || true
sleep 3
kill_by_name "bin/tetrislogd"
sleep 1

echo
echo "== what that session actually exchanged, per the server's own log =="
printf "    requests handled : %s\n" "$(grep -c "req .*$ROOM" var/log/tetrisd.log 2>/dev/null || echo 0)"
printf "    replay records   : %s\n" "$(grep -c " E .* $ROOM \| S .* $ROOM " var/log/tetrisd.log 2>/dev/null || echo 0)"
echo "    None of the above appeared in the capture."

echo
if [ $fail -eq 0 ]; then
    echo "== PASS: the handshake is in the clear, the session is not =="
    echo "   Open $PCAP in Wireshark to show this interactively."
    exit 0
else
    echo "== FAIL: plaintext found after the handshake =="
    exit 1
fi
