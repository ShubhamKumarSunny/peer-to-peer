#!/bin/bash
# Short commands for running and testing the project by hand.
#   ./run.sh help
# This is only a helper for testing. The tracker and the client do not use it.

ROOT=$(cd "$(dirname "$0")" && pwd)
P2P=${P2P:-$HOME/p2p}          # the playground: one folder per client
INFO=$P2P/tracker_info.txt
EVIL=$P2P/evil                 # a self-made key and certificate, for the attack tests

port_of() {
    case "$1" in
        alice) echo 5001 ;; bob) echo 5002 ;; carol) echo 5003 ;; dave) echo 5004 ;;
        *) echo "" ;;
    esac
}

tracker_addr() { sed -n "${1}p" "$INFO"; }

need_setup() {
    if [ ! -f "$INFO" ]; then echo "Run first:  ./run.sh setup"; exit 1; fi
}

evil_cert() {
    mkdir -p "$EVIL"
    [ -f "$EVIL/c.pem" ] || openssl req -x509 -newkey rsa:2048 -nodes -days 30 -subj "/CN=p2p-tracker-1" \
        -keyout "$EVIL/k.pem" -out "$EVIL/c.pem" 2>/dev/null
}

case "$1" in

setup)
    make -C "$ROOT/tracker" || exit 1
    make -C "$ROOT/client" || exit 1
    [ -f "$ROOT/ca_cert.pem" ] || make -C "$ROOT/tracker" cert || exit 1
    mkdir -p "$P2P/trackers" "$P2P/alice" "$P2P/bob" "$P2P/carol" "$P2P/dave"
    cp "$ROOT/tracker_info.txt" "$ROOT/ca_cert.pem" "$ROOT"/tracker1_*.pem "$ROOT"/tracker2_*.pem "$P2P/"
    [ -f "$P2P/alice/tiny.txt" ] || echo "hello world" > "$P2P/alice/tiny.txt"
    [ -f "$P2P/alice/movie.bin" ] || head -c 3000000 /dev/urandom > "$P2P/alice/movie.bin"
    echo
    echo "Ready. Playground: $P2P"
    echo "Test files in $P2P/alice: tiny.txt (12 bytes), movie.bin (3,000,000 bytes)"
    ;;

t1|t2)
    need_setup
    cd "$P2P/trackers" && exec "$ROOT/tracker/tracker" "$INFO" "${1#t}"
    ;;

alice|bob|carol|dave)
    need_setup
    cd "$P2P/$1" && exec "$ROOT/client/client" "127.0.0.1:$(port_of "$1")" "$INFO"
    ;;

reset)
    if pgrep -x tracker >/dev/null || pgrep -x client >/dev/null; then
        echo "Stop the trackers and clients first (type quit in each window)."; exit 1
    fi
    rm -f "$P2P/trackers/oplog.txt"
    for u in alice bob carol dave; do
        for f in "$P2P/$u"/.shared_*; do [ -f "$f" ] && rm -f "$f"; done
    done
    echo "All users, groups and shares are forgotten. Files in the client folders are kept."
    ;;

check)
    need_setup
    file=${2:-movie.bin}
    for u in bob carol; do
        if [ ! -f "$P2P/$u/$file" ]; then echo "$u: no $file yet"
        elif cmp -s "$P2P/alice/$file" "$P2P/$u/$file"; then echo "$u: $file is IDENTICAL to alice's"
        else echo "$u: $file is DIFFERENT from alice's"; fi
    done
    ;;

oplog)
    cut -c1-110 "$P2P/trackers/oplog.txt"
    ;;

passwords)
    log=$P2P/trackers/oplog.txt
    [ -f "$log" ] || { echo "No oplog yet: create a user first."; exit 1; }
    shift
    words=${*:-pw1 pw2 pw3}
    for w in $words; do
        echo "lines of the oplog that contain \"$w\": $(grep -c -F -- "$w" "$log")"
    done
    echo
    echo "What the oplog has for accounts and logins:"
    grep 'CREATE_USER\|LOGIN' "$log" | cut -c1-90
    ;;

plain)
    need_setup
    echo "Sending a command to tracker 1 WITHOUT TLS. Reply:"
    printf 'CREATE_USER eve pw\n' | nc -w 2 127.0.0.1 "$(tracker_addr 1 | cut -d: -f2)" | xxd
    echo "(15 03 03 ... is a TLS alert: refused. The tracker window says: TLS handshake failed)"
    ;;

stranger)
    need_setup; evil_cert
    file=${2:-movie.bin}; group=${3:-movies}
    ask() { printf '%s\n' "$2" | openssl s_client -quiet $1 -connect 127.0.0.1:5001 2>/dev/null | xxd | head -1; }
    own="-cert $EVIL/c.pem -key $EVIL/k.pem"
    echo "Asking alice's seeder (port 5001) as a stranger. ffff ffff = refused."
    echo "1. a file she never shared:";            ask ""     "GET_PIECE /etc/passwd 0 $group"
    echo "2. $file, no certificate shown:";        ask ""     "GET_PIECE $file 0 $group"
    echo "3. $file, old request without a group:"; ask "$own" "GET_PIECE $file 0"
    echo "4. $file, with a key of my own:";        ask "$own" "GET_PIECE $file 0 $group"
    echo "5. $file, wrong group:";                 ask "$own" "GET_BITFIELD $file other"
    ;;

faketracker)
    need_setup; evil_cert
    mkdir -p "$P2P/viafake" && echo "127.0.0.1:4400" > "$P2P/viafake/tracker_info.txt" && cp "$P2P/ca_cert.pem" "$P2P/viafake/"
    openssl s_server -quiet -accept 4400 -cert "$EVIL/c.pem" -key "$EVIL/k.pem" > "$P2P/viafake/got.txt" 2>&1 &
    fake=$!
    sleep 1
    echo "A fake tracker with a self-made certificate is on port 4400. Starting a client against it:"
    (cd "$P2P/dave" && "$ROOT/client/client" 127.0.0.1:5004 ../viafake/tracker_info.txt < /dev/null 2>&1 | grep -v SEEDER)
    kill $fake 2>/dev/null; wait $fake 2>/dev/null
    echo "Commands the fake tracker received from the client: $(grep -a -c '[A-Z]_[A-Z]\|LOGIN' "$P2P/viafake/got.txt")"
    ;;

fakesync)
    need_setup; evil_cert
    port=$(tracker_addr 1 | cut -d: -f2)
    msg='TRACKER_HELLO\nOP 1 CREATE_USER hacker pw\n'
    echo "Pretending to be the other tracker, no certificate:"
    printf "$msg" | openssl s_client -quiet -connect 127.0.0.1:$port >/dev/null 2>&1
    echo "  -> look at the tracker 1 window: Rejected tracker sync link"
    echo "Pretending to be the other tracker, self-made certificate:"
    printf "$msg" | openssl s_client -quiet -cert "$EVIL/c.pem" -key "$EVIL/k.pem" -connect 127.0.0.1:$port 2>&1 | grep -o "alert unknown ca" | head -1
    echo "  -> look at the tracker 1 window: TLS handshake failed"
    ;;

spy)
    need_setup
    port=$(tracker_addr 1 | cut -d: -f2)
    user=spytest$RANDOM; pass=SuperSecret99
    mkdir -p "$P2P/viarelay" && echo "127.0.0.1:4300" > "$P2P/viarelay/tracker_info.txt" && cp "$P2P/ca_cert.pem" "$P2P/viarelay/"
    python3 "$ROOT/tools/mitm_relay.py" 4300 "$port" "$P2P/viarelay/wire.bin" > /dev/null &
    relay=$!
    sleep 1
    echo "A listener sits between a client and tracker 1 and records everything."
    echo "The client creates user $user with password $pass and logs in:"
    (cd "$P2P/dave" && (echo "create_user $user $pass"; echo "login $user $pass"; sleep 1; echo quit) |
        "$ROOT/client/client" 127.0.0.1:5004 ../viarelay/tracker_info.txt 2>&1 | grep "created\|Login")
    kill $relay 2>/dev/null; wait $relay 2>/dev/null
    echo "Bytes the listener recorded:            $(wc -c < "$P2P/viarelay/wire.bin" | tr -d ' ')"
    echo "Times the password appears in them:     $(grep -a -c "$pass" "$P2P/viarelay/wire.bin")"
    echo "Times the word LOGIN appears in them:   $(grep -a -c LOGIN "$P2P/viarelay/wire.bin")"
    ;;

newkeys)
    need_setup
    make -s -C "$ROOT/tracker" cert 2>/dev/null || exit 1
    cp "$ROOT"/tracker1_*.pem "$ROOT"/tracker2_*.pem "$P2P/"
    echo "Both trackers have new keys and certificates. ca_cert.pem did not change."
    echo "A tracker uses its new key the next time it is started (quit, then ./run.sh t1 or t2)."
    ;;

leaks)
    need_setup
    user=leaktest$RANDOM
    echo "Running a client under the leak checker (create user, login, list_groups, quit)..."
    (cd "$P2P/dave" && (echo "create_user $user pw"; echo "login $user pw"; echo list_groups; sleep 2; echo quit) |
        leaks --atExit -- "$ROOT/client/client" 127.0.0.1:5004 "$INFO" 2>&1 | grep -a "leaks for")
    ;;

*)
    cat <<EOF
Start
  ./run.sh setup         build everything, make certificates, make the test folders
  ./run.sh t1            start tracker 1        (one window each)
  ./run.sh t2            start tracker 2
  ./run.sh alice         start a client: alice, bob, carol or dave
  ./run.sh reset         forget all users, groups and shares (stop everything first)

Check
  ./run.sh check [file]      is bob's and carol's copy identical to alice's? (default movie.bin)
  ./run.sh oplog             show the trackers' log
  ./run.sh passwords [words] is a password in the log? (default: pw1 pw2 pw3)
  ./run.sh leaks             memory leak check of the client (macOS)

Attack
  ./run.sh plain         talk to the tracker without TLS
  ./run.sh stranger      ask alice's seeder for a file without being a member
  ./run.sh faketracker   a fake tracker; does a client trust it?
  ./run.sh fakesync      pretend to be the other tracker
  ./run.sh spy           listen between a client and the tracker

Keys
  ./run.sh newkeys       give both trackers new keys (clients keep ca_cert.pem)
EOF
    ;;
esac
