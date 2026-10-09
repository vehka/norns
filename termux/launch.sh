#!/data/data/com.termux/files/usr/bin/bash
# what the home screen widget runs: start norns, or just bring up its window
# when it is already running
. "$(dirname "$0")/env.sh"

# a widget task has no terminal
if [ ! -t 1 ]; then
    mkdir -p "$NORNS_LOG"
    exec >> "$NORNS_LOG/launch.log" 2>&1
fi

if pgrep -f 'build/norns/norns' > /dev/null && pgrep -x jackd > /dev/null; then
    exec "$NORNS_DIR/termux/show.sh"
fi
exec "$NORNS_DIR/termux/start.sh"
