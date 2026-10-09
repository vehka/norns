#!/data/data/com.termux/files/usr/bin/bash
# build norns natively under Termux (android, no root).
#
# packaged dependencies come from pkg; nng, libmonome, the jack command-line
# tools and a python-3.12+-capable waf are not packaged and are built into
# $NORNS_DEPS (default ~/norns-deps). safe to re-run.
set -e
. "$(dirname "$0")/env.sh"

PREFIX_DEPS="$NORNS_DEPS/prefix"
SRC="$NORNS_DEPS/src"
WAF="$NORNS_DEPS/waf"
mkdir -p "$SRC" "$PREFIX_DEPS/bin"

pkg install -y x11-repo
pkg install -y supercollider jack2 liblo lua53 libcairo libsndfile alsa-lib glib \
    sdl2 termux-gui-c libevdev ncurses readline clang cmake ninja pkg-config python git

[ -f "$WAF" ] || curl -fsSL -o "$WAF" https://waf.io/waf-2.1.4

# nng 1.x (the 2.x API is not compatible), static
if [ ! -f "$PREFIX_DEPS/lib/libnng.a" ]; then
    [ -d "$SRC/nng" ] || git clone --depth 1 -b v1.12.4 https://github.com/nanomsg/nng "$SRC/nng"
    cmake -S "$SRC/nng" -B "$SRC/nng/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX_DEPS" -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DNNG_TESTS=OFF -DNNG_TOOLS=OFF
    ninja -C "$SRC/nng/build" install
fi

# libmonome without udev. its example programs fail -Werror under clang after
# the library itself is built, hence -k and the explicit check
if [ ! -f "$PREFIX_DEPS/lib/libmonome.so" ]; then
    [ -d "$SRC/libmonome" ] || git clone --depth 1 https://github.com/monome/libmonome "$SRC/libmonome"
    (cd "$SRC/libmonome" &&
        python3 "$WAF" configure --prefix="$PREFIX_DEPS" --disable-udev &&
        { python3 "$WAF" build -k || true; } &&
        { python3 "$WAF" install -k || true; })
    [ -f "$PREFIX_DEPS/lib/libmonome.so" ] || { echo "libmonome build failed" >&2; exit 1; }
fi

# jack_connect is what sc/core/Crone.sc shells out to; termux's jack2 has no tools
if [ ! -x "$PREFIX_DEPS/bin/jack_connect" ]; then
    [ -d "$SRC/jack-example-tools" ] ||
        git clone --depth 1 https://github.com/jackaudio/jack-example-tools "$SRC/jack-example-tools"
    for t in connect lsp; do
        clang -O2 -w -D__PROJECT_VERSION__='"4"' -o "$PREFIX_DEPS/bin/jack_$t" \
            "$SRC/jack-example-tools/tools/$t.c" $(pkg-config --cflags --libs jack)
    done
    ln -sf jack_connect "$PREFIX_DEPS/bin/jack_disconnect"
fi

# non-interactive REPL client (see termux/repl-send.c)
clang -O1 -I"$PREFIX_DEPS/include" -o "$PREFIX_DEPS/bin/repl-send" \
    "$NORNS_DIR/termux/repl-send.c" "$PREFIX_DEPS/lib/libnng.a" -latomic

# audio output: plays jack's monitor ports through AAudio (android api 26+)
clang -O2 -target "$(clang -dumpmachine | sed 's/[0-9]*$//')26" -o "$PREFIX_DEPS/bin/jack-aaudio" \
    "$NORNS_DIR/termux/jack-aaudio.c" -ljack -laaudio

cd "$NORNS_DIR"
git submodule update --init --recursive
export CPPFLAGS="-I$PREFIX_DEPS/include"
export LDFLAGS="-L$PREFIX_DEPS/lib -Wl,-rpath,$PREFIX_DEPS/lib"
python3 "$WAF" configure --desktop
python3 "$WAF" build --desktop

# runtime layout: the lua side expects ~/norns and ~/dust
[ -e "$HOME/norns" ] || ln -s "$NORNS_DIR" "$HOME/norns"
mkdir -p "$HOME/dust/code" "$HOME/dust/data" "$HOME/dust/audio/tape"
mkdir -p "$HOME/.local/share/SuperCollider/Extensions"
cp sc/norns-config.sc "$HOME/.local/share/SuperCollider/Extensions/"
# screen and controls in a Termux:GUI window; without the Termux:GUI app
# installed this only logs a warning and norns runs headless
[ -e "$HOME/matronrc.lua" ] || echo "_boot.add_io('screen:tgui', {})" > "$HOME/matronrc.lua"

echo "built. sclang needs one throwaway start to register the norns class paths;"
echo "termux/start.sh does the rest."
