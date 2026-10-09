#!/data/data/com.termux/files/usr/bin/bash
# bring up the norns window (screen:tgui): reopen it if it was closed, raise
# it if it is in the background
. "$(dirname "$0")/env.sh"
repl-send ws4://127.0.0.1:5555 '_norns.screen_tgui_show()' 1
