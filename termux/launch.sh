#!/data/data/com.termux/files/usr/bin/bash
# what the home screen widget runs: start norns, or just bring up its window
# when it is already running. with NORNS_AUDIO_INPUT set (the norns-fast
# shortcut), a norns running with another audio input is restarted
. "$(dirname "$0")/env.sh"

# a widget task has no terminal
if [ ! -t 1 ]; then
    mkdir -p "$NORNS_LOG"
    exec >> "$NORNS_LOG/launch.log" 2>&1
fi

if pgrep -f 'build/norns/norns' > /dev/null && pgrep -x jackd > /dev/null; then
    # the input is the last argument of jack-aaudio
    input="$(pgrep -a -x jack-aaudio | awk '{print $NF; exit}')"
    if [ -z "${NORNS_AUDIO_INPUT:-}" ] || [ "$NORNS_AUDIO_INPUT" = "$input" ]; then
        exec "$NORNS_DIR/termux/show.sh"
    fi
    echo "audio input is $input, restarting with $NORNS_AUDIO_INPUT"
fi
exec "$NORNS_DIR/termux/start.sh"
