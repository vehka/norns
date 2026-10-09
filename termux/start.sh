#!/data/data/com.termux/files/usr/bin/bash
# start jack, sclang and norns (in that order) in the background.
# logs go to $NORNS_LOG ($TMPDIR/norns-run by default).
#
# matron REPL:  ws://<phone>:5555   sclang REPL: ws://<phone>:5556
#   e.g. build/maiden-repl/maiden-repl
. "$(dirname "$0")/env.sh"

"$NORNS_DIR/termux/stop.sh"
mkdir -p "$NORNS_LOG"
cd "$NORNS_DIR" || exit 1

# keep android from suspending the audio processes with the screen off
command -v termux-wake-lock >/dev/null && termux-wake-lock

wait_for() { # file, pattern, seconds
    for _ in $(seq 1 "$3"); do
        grep -qE "$2" "$1" 2>/dev/null && return 0
        sleep 1
    done
    return 1
}

# jackd runs on a timer (the dummy driver) and jack-aaudio plays its monitor
# ports through android's audio; termux's own opensles driver crackles.
# NORNS_JACK_DRIVER=opensles goes back to it. either way, a client that dies
# uncleanly leaves the server unusable, which is why stop.sh always takes
# jack down too
if [ "${NORNS_JACK_DRIVER:-dummy}" = dummy ]; then
    nohup jackd -d dummy -m -r 48000 -p "${NORNS_JACK_PERIOD:-960}" \
        > "$NORNS_LOG/jack.log" 2>&1 &
    sleep 2
    nohup jack-aaudio "${NORNS_AUDIO_BUFFER_MS:-60}" > "$NORNS_LOG/aaudio.log" 2>&1 &
    sleep 1
else
    nohup jackd -d opensles ${NORNS_JACK_ARGS:-} > "$NORNS_LOG/jack.log" 2>&1 &
    sleep 3
fi

# sclang must own port 57120 before matron starts its handshake
nohup build/ws-wrapper/ws-wrapper ws://0.0.0.0:5556 sclang \
    > "$NORNS_LOG/sclang.log" 2>&1 < /dev/null &
if ! wait_for "$NORNS_LOG/sclang.log" 'AudioContext: initPolls' 60; then
    echo "sclang did not come up; see $NORNS_LOG/sclang.log" >&2
    exit 1
fi

# SYSTEM > SLEEP and RESTART in the norns menu: there is no systemd and no
# powering off here, so they stop or restart this whole stack (see
# lua/core/norns.lua). detached, because the stack they run in gets killed
export NORNS_LOG
export NORNS_SHUTDOWN_CMD="setsid nohup $NORNS_DIR/termux/stop.sh > /dev/null 2>&1 < /dev/null &"
export NORNS_RESTART_CMD="setsid nohup $NORNS_DIR/termux/start.sh > $NORNS_LOG/restart.log 2>&1 < /dev/null &"

nohup build/ws-wrapper/ws-wrapper ws://0.0.0.0:5555 build/norns/norns \
    > "$NORNS_LOG/norns.log" 2>&1 < /dev/null &
if wait_for "$NORNS_LOG/norns.log" 'norns.startup_status.ok' 40; then
    echo "norns is up (logs in $NORNS_LOG)"
else
    echo "norns did not report startup ok; see $NORNS_LOG/norns.log" >&2
    exit 1
fi
