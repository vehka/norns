#!/data/data/com.termux/files/usr/bin/bash
# reopen the norns window (screen:tgui) after it has been closed
. "$(dirname "$0")/env.sh"
repl-send ws4://127.0.0.1:5555 '_norns.screen_tgui_show()' 1
