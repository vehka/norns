# Building & running norns on desktop Linux (x86-64)

This documents how to build and run norns on a generic x86-64
Linux box (tested on **Manjaro**, kernel 6.12, Python 3.14, GCC 15-era
toolchain, libgpiod v2, Lua 5.3).

It has also been built and tested on **Ubuntu 22.04.5 LTS** (kernel 6.8,
Python 3.10, GCC 11.4, libgpiod v1, liblo 0.31, Lua 5.3). Ubuntu/Debian
differs from Arch/Manjaro in several places — see
[§1a Ubuntu / Debian (apt)](#1a-ubuntu--debian-apt) for the apt package
list and the platform-specific gotchas, which are cross-referenced from the
Arch instructions below.

---

## 1. Prerequisites

### System packages (Manjaro / Arch, from `extra`)

```
sudo pacman -S --needed nng liblo lua53 avahi libgpiod libevdev glib2 \
                        sdl2 cairo jack2 alsa-lib libsndfile
```

(`jack2` or a PipeWire-JACK provider; `cairo`, `sdl2`, `alsa-lib`,
`libsndfile`, `glib2` are usually already present. `ncurses` is only needed by
the optional `maiden-repl` CLI and is normally already installed — don't force
an upgrade if it conflicts with `lib32-ncurses`.)

Notes on naming differences from the Debian-oriented `readme-setup.md`:
- IPC uses **`nng`**, not `nanomsg` (`libnng-dev` on Debian).
- Arch's `lua53` package provides `lua53.pc`, so `pkg-config lua53` works
  as-is — no wscript change needed.

### 1a. Ubuntu / Debian (apt)

Tested on **Ubuntu 22.04.5 LTS**. All dependencies are in the stock repos
(`main`/`universe`); no PPAs or source builds beyond `libmonome`.

```sh
sudo apt-get install -y \
  libevdev-dev liblo-dev libudev-dev libcairo2-dev liblua5.3-dev \
  libavahi-compat-libdnssd-dev libasound2-dev libsndfile1-dev \
  libjack-jackd2-dev libnng-dev libgpiod-dev libglib2.0-dev \
  libsdl2-dev libncurses5-dev libncursesw5-dev
```

(`libncurses*-dev` is only for the optional `maiden-repl`. Either
`libjack-jackd2-dev` or `libjack-dev` provides `jack.pc`.)

How Ubuntu 22.04 differs from Manjaro — each point is expanded where it
applies later in this doc:

- **`pkg-config lua53` works as-is.** Ubuntu's `liblua5.3-dev` ships a
  `lua53.pc` (alongside `lua5.3.pc`), so no wscript change is needed — same
  end result as Arch, despite the `.pc` naming you might expect on Debian.
- **`nng` has no `.pc`**, but the wscript finds it via `check_cc`
  (lib + `nng/nng.h`), so `libnng-dev` is sufficient.
- **libgpiod is v1** (1.6.3) here, not v2 — same major as the Pi. The
  desktop port stubs GPIO under `NORNS_DESKTOP`, so only the lib's presence
  matters; v1 is fine.
- **Use the bundled `./waf`** — no waf-2.1 download needed. Ubuntu 22.04 has
  Python 3.10, which still has the `imp` module that waf 2.0.14 imports
  (removed only in 3.12+, the Manjaro case). But there is no bare `python`
  binary, so invoke it as **`python3 waf ...`** (or
  `sudo apt install python-is-python3` and use `./waf` directly). The same
  applies to libmonome's `./waf` → `python3 ./waf ...`.
- **`/usr/local/lib` is already on the linker path** (via
  `/etc/ld.so.conf.d/libc.conf`), so the manual `ld.so.conf.d` step in the
  libmonome section is unnecessary — just run `sudo ldconfig` after
  `make install` to refresh the cache.
- **Two source fixes were needed** that Manjaro's newer toolchain/liblo did
  not trigger (both already applied in this fork, harmless on Arch):
  - `matron/src/event_types.h` mirrored a newer liblo's
    `typedef struct lo_message_ *lo_message`, which **conflicts with liblo
    0.31's `typedef void *lo_message`**. Fixed by including
    `<lo/lo_types.h>` instead of hand-rolling the typedef, so it tracks
    whatever liblo is installed.
  - `maiden-repl` failed under Ubuntu GCC's default
    `-Werror=format-security` hardening: `wprintw(pad, txt)` /
    `mvwprintw(..., page_title[i])` pass a non-literal format string. Fixed
    to `wprintw(pad, "%s", txt)` / `mvwprintw(..., "%s", page_title[i])`.

The rest of this doc's flow (submodules, SuperCollider, build, launch) is
identical; just substitute `python3 waf` for `./waf` and the apt package
list above for the `pacman` one.

### libmonome (not packaged — build from source)

```
git clone https://github.com/monome/libmonome
cd libmonome
./waf configure && ./waf build
sudo ./waf install
```

Installs `libmonome.so` to `/usr/local/lib` and `monome.h` to
`/usr/local/include`. The header is on the default compile search path, but on
Arch **`/usr/local/lib` is not on the runtime linker path** — without the next
step you get `error while loading shared libraries: libmonome.so.1` at launch.
Register it and refresh the cache:

```
echo '/usr/local/lib' | sudo tee /etc/ld.so.conf.d/usrlocal.conf
sudo ldconfig
```

Verify with `ldconfig -p | grep monome`.

### SuperCollider

Install `supercollider` (the `supercollider` apt package, or Arch's
`supercollider`, pulls `sclang` + `scsynth`), then register the norns SC
classes by running the bundled installer **from the `sc/` dir**:

```sh
cd sc && ./install.sh && cd ..
```

This copies **only** `norns-config.sc` into
`~/.local/share/SuperCollider/Extensions/`. That one file is the whole
mechanism: on class-library compile it calls `LanguageConfig.addIncludePath`
to point SC at `~/norns/sc/core`, `~/norns/sc/engines` and `~/dust` — i.e.
the norns classes are loaded **in place from the repo** (via the `~/norns`
symlink created in §3), not copied into the Extensions tree.

> ⚠️ **Do not also copy or symlink `sc/core` / `sc/engines` into the
> Extensions dir.** Because `norns-config.sc` already adds them as include
> paths, a second copy under `Extensions/` makes SC scan every class twice
> and abort at startup with e.g.
> `ERROR: duplicate Class found: 'CronePoll'`. (This is *not* desktop- or
> distro-specific — it bites on Arch too; the upstream Pi packaging that the
> `monome-norns-*` Debian packages came from installs the classes a
> different way and is not how the desktop/source build is meant to be set
> up.) If you hit the duplicate error, delete the extra copies from
> `~/.local/share/SuperCollider/Extensions/` (keep only `norns-config.sc`)
> and recompile sclang.

### Submodules

```
git submodule update --init --recursive
```

Required — the build references `crone/softcut`, `third-party/link`
(+ its `asio-standalone`), `concurrentqueue`, `readerwriterqueue`,
`lua-cjson`, etc. Nothing builds without them.

### waf (Python 3.12+)

The bundled `waf` 2.0.14 does `import imp`, which was removed in **Python
3.12+**, so it fails to start on modern Python. The Raspberry-Pi targets ship
Python ≤3.11, so the repo keeps the upstream 2.0.14 unchanged.

On a Python 3.12+ box, fetch a newer waf (2.0.25+; any 2.1.x works) and run it
directly instead of `./waf`:

```sh
wget https://waf.io/waf-2.1.4 -O waf-2.1
python3 waf-2.1 configure --desktop
python3 waf-2.1 build --desktop
```

Keep it out of version control with a per-clone local ignore (not the tracked
`.gitignore`):

```sh
echo waf-2.1 >> .git/info/exclude
```

The project `wscript`s use stable waf API, so a newer waf is a drop-in; the
`./waf ...` commands elsewhere in this doc become `python3 waf-2.1 ...`.

---


## 2. Build

```
./waf configure --desktop
./waf build --desktop
```

Produces:
- `build/norns/norns`   — combined matron + crone + sidecar binary
- `build/ws-wrapper/ws-wrapper` — wraps a process's stdio in websockets (for Maiden)
- `build/watcher/watcher`
- `build/maiden-repl/maiden-repl` — optional CLI REPL

To rebuild after editing sources, just `./waf build --desktop`. Changes to a
`wscript` `configure` step require re-running `./waf configure --desktop`.

---

## 3. Launching

### One-time runtime setup

The Lua runtime hardcodes paths under `$HOME`: `weaver.cc` bootstraps from
`$HOME/norns/lua/core/config.lua` (overridable via `NORNS_CONFIG`), and
`config.lua` builds `package.path` from `$HOME/norns/lua` and uses `$HOME/dust`
for scripts/data. Since this checkout lives elsewhere, symlink it and create
the dust tree:

```
ln -s "$(pwd)" ~/norns        # run from the repo root
mkdir -p ~/dust/code ~/dust/data ~/dust/audio/tape

# Several scripts hardcode /home/we (the upstream Pi user). Symlink it to $HOME:
sudo ln -s "$HOME" /home/we
```

Create a desktop `matronrc.lua`. `matron` loads `$HOME/matronrc.lua` first
(falling back to `$HOME/norns/matronrc.lua`), so a file in `$HOME` overrides
the repo's Pi-oriented one (the repo `matronrc.lua` keeps the upstream Pi
`init_norns()` default and is **not** the desktop config). Copy the bundled
example:

```sh
cp matronrc.lua.desktop ~/matronrc.lua
```

It opens the SDL window (512x256, mirroring the 128x64 screen) and documents
the keyboard input options inline. The minimal form is just:

```lua
-- ~/matronrc.lua
_boot.add_io('screen:sdl', {})
```

Keyboard input is built into the `screen:sdl` backend (hold Alt for the
encoder/key control grid; other keys type) and is configurable via an
`input = {...}` subtable — see `matronrc.lua.desktop` and §6.

For a **truly headless** run (no window), use an empty `~/matronrc.lua` — no
screen/input IO is registered and `screen.*` calls are harmless. (Control via
MIDI / OSC / Maiden REPL.)

### Connecting a grid (serial-device permissions)

`dev_monitor` (`device_monitor.cc`) identifies a grid it by udev properties —
a current grid reports `ID_VENDOR=monome` / `ID_MODEL=grid`, so
`is_dev_monome_grid()` matches with no extra config. The catch is
**node permissions**: on Arch/Manjaro the grid comes up as

```
crw-rw---- 1 root uucp /dev/ttyACM0
```

i.e. owned by group `uucp`. If your user isn't in `uucp`, `monome_open()` in
`device_monome.cc` fails (`error: couldn't open monome device at ...`) even
though udev detected it — the grid just silently never appears. Check with
`ls -l /dev/ttyACM0` and `id` (look for `uucp`). Two ways to fix:

**A — add yourself to `uucp` (simplest):**

```sh
sudo usermod -aG uucp $USER
# then fully log out and back in (newgrp uucp only affects the current shell)
```

**B — a udev rule (no group juggling; grants to the local-seat user):**

```sh
# /etc/udev/rules.d/70-monome.rules
SUBSYSTEM=="tty", ATTRS{idVendor}=="0483", ATTRS{idProduct}=="5740", ATTRS{manufacturer}=="monome", MODE="0660", TAG+="uaccess"
```

```sh
sudo udevadm control --reload-rules && sudo udevadm trigger   # then replug
```

### Run sequence (verified working)

The matron and SuperCollider REPLs are exposed over **nng websockets** by
`ws-wrapper`, which *launches* the target as a child and wraps its stdio (so
don't run `norns`/`sclang` bare if you want Maiden). The expected endpoints
(from `maiden-repl/src/io.c`) are **matron → `ws://<host>:5555/`** and
**sclang → `ws://<host>:5556/`**.

**Order matters.** matron only performs the SuperCollider handshake during its
first ~12 s (it retries `/ready` to sclang on port 57120, then gives up). So
sclang must already own port 57120 *before* norns starts, or you get
`SUPERCOLLIDER FAIL`. Start each in its own terminal, in this order:

```
# 0. JACK (or PipeWire-JACK). Confirm with: jack_lsp
jack_control start

# 1. sclang FIRST -- under ws-wrapper, on 5556.
#    Wait for the "Norns startup / OSC rx port: 57120" banner + engine list.
build/ws-wrapper/ws-wrapper ws://0.0.0.0:5556 sclang

# 2. THEN norns (combined matron+crone), under ws-wrapper, on 5555.
build/ws-wrapper/ws-wrapper ws://0.0.0.0:5555 build/norns/norns
```

On success, matron prints `norns.startup_status.ok` (a timeout instead means
the handshake failed — see ports below).
