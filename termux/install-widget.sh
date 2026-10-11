#!/data/data/com.termux/files/usr/bin/bash
# install Termux:Widget shortcuts: "norns" starts norns (or brings up its
# window), "norns-fast" does the same with the low latency audio input
# (which breaks up with the screen off), "norns-stop" stops it. they go in
# tasks/, so they run without
# opening a terminal. add the Termux:Widget widget to the home screen, or
# refresh it, to see them
. "$(dirname "$0")/env.sh"

dir="$HOME/.shortcuts/tasks"
mkdir -p "$dir"
chmod 700 "$HOME/.shortcuts" "$dir"

shortcut() { # name, script, environment
    printf '#!%s/bin/bash\n%sexec "%s/termux/%s"\n' "$PREFIX" "${3:+export $3
}" "$NORNS_DIR" "$2" > "$dir/$1"
    chmod 700 "$dir/$1"
}
shortcut norns launch.sh
shortcut norns-fast launch.sh NORNS_AUDIO_INPUT=fast
shortcut norns-stop stop.sh
echo "installed $dir/norns, $dir/norns-fast and $dir/norns-stop"
