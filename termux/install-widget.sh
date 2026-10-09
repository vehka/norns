#!/data/data/com.termux/files/usr/bin/bash
# install Termux:Widget shortcuts: "norns" starts norns (or brings up its
# window), "norns-stop" stops it. they go in tasks/, so they run without
# opening a terminal. add the Termux:Widget widget to the home screen, or
# refresh it, to see them
. "$(dirname "$0")/env.sh"

dir="$HOME/.shortcuts/tasks"
mkdir -p "$dir"
chmod 700 "$HOME/.shortcuts" "$dir"

shortcut() { # name, script
    printf '#!%s/bin/bash\nexec "%s/termux/%s"\n' "$PREFIX" "$NORNS_DIR" "$2" > "$dir/$1"
    chmod 700 "$dir/$1"
}
shortcut norns launch.sh
shortcut norns-stop stop.sh
echo "installed $dir/norns and $dir/norns-stop"
