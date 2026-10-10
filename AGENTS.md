# norns on Termux (Android) — notes for agents

This branch (`termux`, off `main`) makes the `--desktop` build of norns compile
and run natively in Termux on an unrooted Android phone: built against bionic,
talking to Termux's own `jackd`. It is not a proot/chroot Linux install, and
should not become one — a distro's JACK clients cannot reach the Android-audio
`jackd`.

`build_desktop.md` describes the desktop build this is based on. Read it for
the general flow (start order, `matronrc.lua`, ws-wrapper ports); this file
covers only what differs on Termux.

## State

Working, verified on a Galaxy S23+ (aarch64, Android 16, Termux 0.118.3,
SuperCollider 3.14.1, jack2 1.9.22):

- `build/norns/norns` builds and reaches `norns.startup_status.ok`.
- crone, softcut and SuperCollider are fully wired in JACK; the startup sound
  plays through the phone speaker.
- Engine load, SC polls (`amp_out_l`) and metros (start, count-limited stop,
  cancel on script clear) work. `clock.sleep` / `clock.sync` work.
- `screen:tgui`: the window shows the screen, and K1-K3 / E1-E3 touch pads
  drive the menu and scripts. `awake` (tehn/awake, PolyPerc) runs and plays.
- matron and sclang REPLs work over ws-wrapper.

Not done / not tested:

- `screen:tgui`: reconnect after the plugin dies, reopening a closed window
  and the clean shutdown path are written but not exercised (`stop.sh`
  SIGTERMs matron, which exits without running IO teardown).
- **SDL screen** (`screen:sdl`): compiled in, never run; needs an X server
  (see below).
- Audio input, softcut under load, MIDI, grid, crow, HID.
- `termux/build.sh` was written after the steps were done by hand. It has
  only been syntax-checked, not run end to end.

## Layout

| Path | What |
|---|---|
| `~/norns` | this checkout (the Lua side hardcodes `$HOME/norns`) |
| `~/dust` | scripts and data (`code`, `data`, `audio/tape`) |
| `~/matronrc.lua` | `_boot.add_io('screen:tgui', {})` |
| `~/norns-deps/prefix` | nng (static), libmonome, `jack_connect`, `jack_lsp`, `repl-send`, `jack-aaudio` |
| `~/norns-deps/waf` | waf 2.1.4; the bundled `./waf` fails on Python 3.12+ |
| `~/.local/share/SuperCollider/Extensions/norns-config.sc` | adds the norns SC class paths |
| `~/.local/share/SuperCollider/Extensions/mi-UGens` | `MiPlaits`, built by the emplaitress installer |
| `~/norns-deps/src/mi-UGens` | hand build of all twelve mi-UGens (`build/mi-UGens`), not installed |
| `~/.local/share/SuperCollider/Extensions/sc3-plugins` | `JPverb`, `Greyhole`, `DistortionUGens` (`Decimator` and others) |
| `~/norns-deps/src/sc3-plugins` | `supercollider/sc3-plugins` with its submodules; only those three targets built |
| `~/src/norns-deps` | `vehka/norns-deps`, on its `termux` branch (see Scripts, mods and UGens) |
| `~/src/noseda` | the `noseda` agent skill, linked into `~/.claude/skills` |
| `$TMPDIR/norns-run/` | `jack.log`, `aaudio.log`, `sclang.log`, `norns.log`, `launch.log`, `restart.log` |
| `~/.shortcuts/tasks/` | `norns`, `norns-stop` (Termux:Widget) |

## Build and run

```sh
termux/build.sh     # packages, deps, configure, build, runtime layout
termux/start.sh     # jack -> jack-aaudio -> sclang -> norns; waits for startup ok
termux/stop.sh      # stops everything, including jack
termux/show.sh      # bring up the norns window (reopen or raise it)
termux/launch.sh    # start.sh if norns is not running, else show.sh
termux/install-widget.sh   # Termux:Widget shortcuts (build.sh runs it)
```

**Home screen widget.** `install-widget.sh` writes two wrappers into
`~/.shortcuts/tasks/` (real files; they run without a terminal): `norns`
runs `launch.sh`, `norns-stop` runs `stop.sh`. They show up in the
Termux:Widget widget (`com.termux.widget`) once it is added to the home
screen or refreshed. A task has no output: `launch.sh` logs to
`$TMPDIR/norns-run/launch.log`, and the window appears after about 30 s.

**SLEEP and RESTART in the norns menu.** `lua/core/norns.lua` runs
`$NORNS_SHUTDOWN_CMD` / `$NORNS_RESTART_CMD` instead of `sudo shutdown` and
`systemctl` + self-terminate when they are set. `start.sh` sets them to a
detached `stop.sh` / `start.sh` (the latter logging to `restart.log`), so
SLEEP stops the whole stack and closes the window, and RESTART (also RESET
and the end of UPDATE) restarts all of it, JACK included. Both verified by
calling `norns.shutdown()` / `_norns.restart()` over the REPL.

Rebuild after editing sources:

```sh
P=$HOME/norns-deps/prefix
export CPPFLAGS="-I$P/include" LDFLAGS="-L$P/lib -Wl,-rpath,$P/lib"
python3 ~/norns-deps/waf configure --desktop   # only after wscript changes
python3 ~/norns-deps/waf build --desktop
```

Talking to the REPLs without a terminal UI:

```sh
. termux/env.sh     # puts ~/norns-deps/prefix/bin on PATH
repl-send ws4://127.0.0.1:5555 'print(norns.version.update)' 2   # matron (Lua)
repl-send ws4://127.0.0.1:5556 's.queryAllNodes' 2               # sclang
```

`python3 ~/.claude/skills/noseda/scripts/nrepl.py 'lua'` (from the `noseda`
skill, needs `websocket-client`) reaches matron the same way and takes
`--wait SECONDS`; it has no option for the sclang port.

`build/maiden-repl/maiden-repl` is the interactive equivalent. The REPL
transport is nng bus0 over websocket in text mode (`ws4://`, with
`NNG_OPT_WS_SEND_TEXT` / `RECV_TEXT`); a plain websocket client will not work.

## Gotchas

- **Starting norns makes sound.** The startup chime plays on the phone
  speaker. To test an engine silently, send `audio.level_dac(0)` first, and
  restore with `params:lookup_param("output_level"):bang()`.
- **`pkill -f` kills your own shell** if the pattern appears anywhere in the
  command line you are running, heredocs included (exit code 144). That
  includes the plain paths `build/norns/norns` and
  `build/ws-wrapper/ws-wrapper`, and `start.sh` runs `stop.sh` too. Run
  `termux/start.sh` / `termux/stop.sh` in a command that mentions neither.
  When this happens to `start.sh`, the shell dies but the half-started stack
  lives on; a second `start.sh` on top of it gives two norns instances and
  odd failures (port in use, segfaults). Run `stop.sh` first.
- **`pkill -x norns` does not match.** norns and ws-wrapper are started by
  path, and the sidecar child renames itself to `sidecar [norns]` and ignores
  SIGTERM. `stop.sh` matches command lines and escalates to SIGKILL.
- **JACK does not survive a client crash.** After any client dies uncleanly,
  `jackd` refuses new clients (`jack_client_open() failed; status = 33`).
  Restart the whole stack; `start.sh` always does.
- **sclang must be up before norns**, and needs one throwaway start after
  `norns-config.sc` is first installed (the include paths are only compiled in
  on the next launch).
- **One mod that fails to load stops all of them.** `mods.load` does not
  catch errors, so the mods after it in the scan are not loaded either, and
  the only sign is a traceback after `loading mod:` in `norns.log`. Seen
  with nbout cloned without its `lib/nb` submodule.
- **`norns.script.clear()` sets `note_players` to nil.** nb voices can only
  be tested with a script loaded.
- **`norns.is_shield` is true here and `norns.is_desktop` is nil.** Code
  that asks norns what it runs on takes this for a shield.
- **No `/tmp`, no `sudo`, no `/home/we`.** Use `$TMPDIR`. Community scripts
  that hardcode `/home/we` will break.
- **`/proc/stat`, `/sys/firmware`, `/dev/snd`, `/dev/input` are
  permission-denied.** A node existing does not mean it can be opened; check
  `fopen` results.
- **nng must be 1.x** (v1.12.4). The 2.x API dropped the protocol headers the
  code includes.
- **libmonome's build "fails"** on its example programs (`-Werror` under
  clang) after the library itself is built and installed. That is expected.

## What the branch changes

- `matron/src/compat/android/`
  - `android_compat.h`, `pthread_cancel.cc`: bionic has no `pthread_cancel` /
    `pthread_testcancel`. Emulated as deferred cancellation. The header
    (force-included into every C++ unit via `norns/wscript`) redirects
    `pthread_create` to a wrapper that keeps a list of live threads;
    `pthread_cancel` flags the thread's record and signals it with SIGUSR2
    (empty handler, no `SA_RESTART`) so its blocking call returns `EINTR`;
    the thread exits at its next `pthread_testcancel()`. A thread that has
    already exited, or was not created through the wrapper, gets `ESRCH`.
    Never `pthread_kill` a raw `pthread_t` here: bionic aborts the process
    when the thread is gone, and reuses the value for later threads (the
    first version of the shim did both and crashed matron on K1).
    waf does not track the force-included header: after editing it, delete
    the objects under `build/norns` so everything recompiles.
  - `device_monitor.cc`: no-op replacement for the udev monitor.
  - `dns_sd.h`: stub for avahi's compat header (no mDNS advertising).
- `wscript`, `norns/wscript`: detect `__ANDROID__` (`NORNS_ANDROID`); skip
  libudev, libgpiod, avahi and `watcher`; swap in the compat sources.
- `third-party/wscript`: two clang-only warning-flag fixes for lua-cjson.
- `matron/src/metro.cc`, `hardware/stat.cc`, `hardware/battery.cc`: extra
  `pthread_testcancel()` calls so the loops are cancellable under the shim.
  `metro_cancel()` marks a metro stopped when its thread is already gone.
  The running/stopped status is set only by `metro_init()` and
  `metro_cancel()`; a metro thread marks itself stopped at the end of a
  count-limited run only if the metro was not restarted meanwhile (`gen`).
  Before, a thread that first ran after its metro was stopped marked it
  running again, which left a dead thread id to be cancelled later (the
  `metro_stop(): ... specified thread does not exist` lines, and takt's
  redraw metro dying). Not behind `__ANDROID__`.
- `matron/src/jack_client.cpp`: `jack_client_get_current_time()` uses
  `CLOCK_MONOTONIC` on Android. Termux's jackd never advances
  `jack_frame_time()` (a fresh client reads 0 forever), which froze the whole
  Lua clock: coroutines never woke from `clock.sleep` / `clock.sync`.
- `norns/sidecar.cpp`: IPC socket path honours `$TMPDIR`.
- `crone/src/Client.h`: missing capture ports are a warning, not an abort.
- `matron/src/hardware/platform.cc`, `hardware/stat.cc`: tolerate unreadable
  device-tree and `/proc/stat`.
- `matron/src/hardware/screen/tgui.cc`: the `screen:tgui` backend (below).
  Registered in `hardware/io.cc` and `screens.h`; `weaver.cc` adds
  `_norns.screen_tgui_show()`. All behind `HAVE_TERMUXGUI`, which `wscript`
  sets when building `--desktop` on Android with `libtermuxgui` present.
- `lua/core/norns.lua`: `NORNS_SHUTDOWN_CMD` / `NORNS_RESTART_CMD` (above).
  Unset, as on a real norns, nothing changes.
- `termux/`: `build.sh`, `start.sh`, `stop.sh`, `show.sh`, `launch.sh`,
  `install-widget.sh`, `env.sh`, `repl-send.c`, `jack-aaudio.c`.

**Rule for new threads on Android:** blocking calls are not cancellation
points here. Any thread that something will `pthread_cancel` (and especially
`pthread_join` afterwards, as `dev_delete()` does) must call
`pthread_testcancel()` in its loop after each blocking call, or the join hangs.

## Known limits

- **No audio input.** The Termux app (0.118.3) does not declare
  `RECORD_AUDIO` in its manifest, so there is no microphone permission to
  grant and nothing running as Termux can record. (Termux:GUI does not
  declare it either.) The code is there: `NORNS_AUDIO_INPUT=1
  termux/start.sh` makes `jack-aaudio` open an AAudio input stream and
  serve it on physical `aaudio_in:capture_*` ports, from a second JACK
  client and through a resampler that follows the ring's lowest fill
  level. It is off by default and untested: the stream opens (low latency
  path, 96-frame bursts), then `requestStart` returns
  `AAUDIO_ERROR_DISCONNECTED`, and `aaudio.log` says `no input`. No sample
  has passed through the ring or the resampler. With a Termux that has the
  permission it should only need the flag; before turning it on, mind that
  the monitor level is saved at 0 dB and the phone's microphone hears its
  speaker. The other way in is a small app of its own that records and
  sends PCM to `jack-aaudio` over a local socket.
- **Latency.** About 30 ms of buffering at 48 kHz (see Audio output), set
  by how long Android stalls threads that have no real-time priority.
- **No hardware devices** (USB MIDI, HID, grid, crow) without root. The plan
  is OSC control, TouchOSC for grid emulation, and Bluetooth MIDI from the
  `feat/bl-midi` branch (8 commits ahead of `main`, not merged here; its
  device threads need the `pthread_testcancel()` rule above).
- **Pi-only menus** (wifi, update, password) call `nmcli`, `systemctl`,
  `sudo` and just log errors.
- **Engines.** Only the stock SuperCollider UGens, `MiPlaits` and three
  sc3-plugins targets (`JPverb`, `Greyhole`, `DistortionUGens`) are
  installed; sc3-plugins is not packaged for Termux. Plugins can be built
  against the installed headers (see Scripts, mods and UGens).
- **Android may kill background processes.** `start.sh` takes a
  `termux-wake-lock`; `stop.sh` releases it.

## Scripts, mods and UGens

Installed in `~/dust/code` and enabled in `~/dust/data/system.mods`:

| Mod | Checkout | Notes |
|---|---|---|
| `emplaitress` | `vehka/emplaitress`, branch `build-ugens` | four Plaits voices for nb; builds `MiPlaits` itself |
| `modhousekeeper` | `vehka/modhousekeeper`, branch `fix-submodules` | mod manager; clones mods with their submodules |
| `nbout` | `sixolet/nbout` | MIDI device `17: nb` that plays an nb voice |

Scripts: `awake`, `lylepmills/buoys` (softcut, grid and arc) and `vehka/takt` (engine `Timber_Takt`, which needs
`JPverb` and `Decimator` from sc3-plugins; used by hand with the virtual
grid).

Neither branch is merged to `main` yet. Verified: `awake` with `out` = midi,
`midi out device` = `17: nb` and `nb midi ch 1` = `emplait 1` plays through
emplaitress.

- **norns-deps: use the `termux` branch.** `vehka/norns-deps` is the
  installer library that scripts and mods carry a copy of (`lib/deps.lua`
  and `lib/deps/`). Its `main` does not know Termux: it takes the phone for
  a shield with `apt`, cannot find SuperCollider under `$PREFIX`, and after
  an install offers `sudo shutdown -r now` because JACK has no files in
  `/dev/shm` here. The `termux` branch (checked out in `~/src/norns-deps`)
  fixes these: `pm` is `termux` (`pkg install`, no root), the platform
  counts as desktop, and a restart is `_norns.reset()`, which runs
  `$NORNS_RESTART_CMD`. For any script or mod here that uses the library,
  copy `lib/deps.lua` and `lib/deps/` from that branch, and make library
  fixes there (`lua5.3 tests/run.lua`), not in the copy. This holds until
  the branch is merged.
- **Mods with git submodules** need `git clone --recurse-submodules`, or
  `git submodule update --init --recursive` afterwards. `start.sh` does not
  run maiden, so there is no `;install`; clone into `~/dust/code`.
- **Enabling a mod without the menu:**
  `require('core/mods').set_enabled('<name>', true, true)` over the REPL,
  then restart the stack.
- **A new or changed `.sc` class or UGen needs the whole stack restarted**
  (`termux/start.sh`). sclang compiles everything under `~/dust` and the
  Extensions folder at start, so never leave a second copy of a class file
  in either: a checkout of a UGen repository with `sc/Classes` belongs
  elsewhere (`~/norns-deps/src`, `~/.cache`).
- **Building SuperCollider plugins.** Projects that want a SuperCollider
  source tree (`-DSC_PATH=...`, then `include/plugin_interface`) accept a
  folder whose `include` is a link to `$PREFIX/include/SuperCollider`;
  `~/norns-deps/src/sc-headers` is one. mi-UGens built unchanged this way
  (about a minute for all twelve). Its single-project `CMakeLists.txt`
  files have no `cmake_minimum_required` and fail with the current cmake;
  wrap them with `add_subdirectory`, as the emplaitress recipe does.
  Install into `~/.local/share/SuperCollider/Extensions/<name>`.
- **sc3-plugins** also reads `SCVersion.txt` from the top of `SC_PATH`
  (`sc-headers` has a link to `include/SCVersion.txt`). Configure with
  `-DSC_PATH=$HOME/norns-deps/src/sc-headers -DSUPERNOVA=OFF -DQUARKS=OFF`,
  then build single targets (`make JPverb Greyhole DistortionUGens`; the
  names are in `make help`) and copy each `.so` together with its class
  file from `source/<group>/sc/`. An engine that names a class which is not
  installed stops the whole class library from compiling, so check a new
  script's `.sc` files for UGens before restarting.
- **Measuring instead of listening:** poll `amp_out_l` from Lua
  (`poll.set("amp_out_l", fn)`) with a script loaded that is silent itself.
  `~/dust/code/termux-test` plays a sine, so it will not do.

## Audio output

`start.sh` runs `jackd -d dummy -C 0 -P 0` and `termux/jack-aaudio.c`, a
JACK client whose `aaudio:playback_*` ports (marked physical, so crone
connects to them as to any sound card) feed a ring buffer played by an
AAudio callback stream.

- **The audio device is the clock.** `jack-aaudio` puts jackd into
  freewheel, where it runs cycles as fast as the clients allow, and blocks
  in its process callback until the device has played the ring down to
  `NORNS_AUDIO_BUFFER_MS` (default 20). So JACK runs at the device's rate,
  and after a late cycle it catches up by itself. The dummy driver's timer
  does nothing after that.
- Why this shape: an Android app cannot have real-time threads
  (`ulimit -r` is 0); only the AAudio callback thread gets `SCHED_FIFO`.
  jackd, scsynth and crone are normal threads at nice -20 (`start.sh`,
  `crone/src/Client.h`) and get stalled now and then, 5-20 ms a few times
  a minute, at any nice level. The buffer has to cover the longest stall;
  that is what sets the latency, not the JACK period
  (`NORNS_JACK_PERIOD`, default 256).
- **Nothing may connect to `system:*` ports in freewheel.** jackd does not
  service its own driver's ports there, and a client that depends on them
  never finishes its cycle: the graph stops and new clients time out
  (`Driver is not running`, matron dies on `Cannot open matron-clock
  client`). Hence `-C 0 -P 0`.
- `NORNS_AUDIO_STATS_S=10 termux/start.sh` logs the ring's fill range
  every 10 s in `aaudio.log` (`fill 352..1216`: frames left after the
  worst read, and the most there was). An underrun line means a stall
  longer than the buffer: a burst of silence, no other harm.
- Measured on the S23+: `awake` playing for 45 s, no underruns, lowest
  fill 192 frames (4 ms left of the 20). Played by hand with `plonky`
  (MxSamples) for about 100 minutes: 111 underruns in that time, script
  loads included, and it sounded solid.
- `NORNS_AUDIO_CLOCK=timer` is the older way: jackd on its timer in sync
  mode (`-S -d dummy -m`), `jack-aaudio` reading `system:monitor_*` through
  a resampler that follows the ring's fill level, since the two clocks
  differ. The timer driver drops the time by which a cycle is more than a
  period late, which drains the ring; it underran at 20 ms where the
  device clock does not.
- Before either, jackd ran async with 960-frame periods and a 60 ms ring
  that drifted up to 160 ms before skipping: about 150 ms in all, with a
  click for every late client and every skip.
- `NORNS_JACK_DRIVER=opensles termux/start.sh` goes back to Termux's own
  driver, which crackles: it keeps two OpenSL buffers queued, so one late
  cycle is an audible dropout.
- The AAudio buffer is two bursts (192 frames); a burst is added when the
  device reports an underrun (`device underruns` in `aaudio.log`).
- It reopens the stream when Android reports an error (output device
  change); not exercised.
- `jack_frame_time()` is stuck at 0 here, hence the `CLOCK_MONOTONIC`
  change in `jack_client.cpp`.

## The screen: Termux:GUI

`screen:tgui` shows norns in a window of the **Termux:GUI** app
(`com.termux.gui`, same F-Droid source as Termux) through `libtermuxgui`
(`pkg install termux-gui-c`). No X server is involved.

- Portrait: the screen on top (128x64 upscaled by an integer factor into a
  shared buffer, 1024x512 on the S23+), then three rows of touch pads, keys
  on the left and encoders on the right: `K1 E1` / `K2 E2` / `K3 E3`.
- Landscape: full screen (system bars hidden, back with a swipe from the
  edge), the screen fills the window, and the pads are invisible zones over
  its two ends: K1-K3 stacked on the left, E1-E3 on the right, each
  `zone_width` (default 0.25) of the window wide. The zones cover part of
  the screen picture; the middle takes no touches.
- **Virtual grid.** In landscape there is a page with a 16x8 grid
  drawn into its own shared buffer. matron sees it as a monome device
  (`dev_monome_new_virtual_grid()` in `device_monome.cc`: a `dev_monome`
  with no libmonome handle, whose `refresh` hands the led data to the
  backend), so scripts use it through `grid.connect()` unchanged. Rotation,
  intensity and tilt are ignored. A finger holds the cell it lands on until
  it lifts; sliding does not retrigger.
- **Grid with screen.** The page after the grid shows both, laid out like
  the arc page: a smaller grid (`TGUI_GRID_SCR_SHARE`, 0.6 of the window
  height, so cells of about 26 dp on the S23+) with a smaller copy of the
  norns screen above it, and labelled K1-K3 / E1-E3 zones at the sides.
  Same grid buffer and touch handling as the grid page. Looked at by hand
  on the S23+.
- **Virtual arc.** Two more landscape pages show it. The arc page: four
  rings of 64 leds in a row (a 4:1 buffer of their own; led 1 is at the
  top), a smaller copy of the norns screen above them, and labelled K1-K3
  / E1-E3 zones in what is left at the sides, which work as on the screen
  page. The rings page: only the rings, as large as they fit, between two
  strips like the grid. matron sees a monome
  device made by `dev_monome_new_virtual_arc()`. Its name has to start
  with `monome arc`: that is how `_norns.monome.add` in
  `lua/core/startup.lua` tells an arc from a grid. A finger turns the ring
  it landed on by the angle it moves around that ring's centre, wherever
  it goes afterwards, 1024 ticks to the turn like the device
  (`ARC_TICKS`); clockwise is positive, and nothing is counted right at
  the centre. A touch under 300 ms that stays within 8 dp is the ring's
  key (press and release sent together on lift). Several rings can be
  turned at once.
- Switching pages: the pages are a ring, screen - grid - grid with screen -
  arc - rings (only those that exist: `grid = false` drops the two grid
  pages, `arc = false` the last two). A sideways swipe
  (80 dp, any number of fingers) on the screen picture, which on the screen
  page means its middle part between the zones, goes to the next page when
  it is leftwards and to the previous one when rightwards. A tap on an
  encoder zone (under 300 ms, within 8 dp, no encoder step sent) goes to
  the next page, on the grid with screen and arc pages too. On the grid and rings pages, a tap on
  the strip right of the picture (dark grey, out to the window edges, at
  least 32 dp wide; the picture shrinks to leave them) goes to the next
  page, on the left strip to the previous one. The grid with screen and arc
  pages have the same two strips beside their small screen. Back goes
  to the screen. The strips are
  weighted children of a row, not sized from the configuration: the full
  screen window is wider than `screen_width` says. The tap counts on
  release, and not when it started with a finger on the grid or the rings. The grid and
  the rings themselves take no page gestures: a surface that sends input
  on touch down cannot also take gestures, so page changes belong on areas
  that do nothing else.
- The grid and the arc column are centred in the window by spaces above
  and below them (`tgui_centred_begin()` / `_end()`); a row does not
  centre its children by itself, and the first version sat at the top.
  Sideways the picture sat about 33 dp right of the middle of the display
  in a screenshot from the S23+ (269 px free on the left, 175 px on the
  right). Probably the plugin lays the window out without the camera
  cutout at that end; not confirmed, and the library has no call for it.
- Arc state: the rings draw and turn by touch, and the arc page with keys,
  encoders and screen has been used by hand (with `lylepmills/buoys`).
  The strips beside its small screen and the rings page build without
  errors; nobody has looked at them yet. buoys is softcut only and plays samples from a
  folder chosen in its meta mode; `~/dust/audio` has none, so it is silent
  here.
- The grid shows up as `tgui grid tgui` in SYSTEM > DEVICES > GRID. It is
  not put on a port by itself (the add event comes before `system.state` is
  read, which then names the ports); here port 1 was set once with
  `grid.vports[1].name = "tgui grid tgui"; grid.update_devices(); norns.state.save()`.
  The arc is `monome arc tgui` under SYSTEM > DEVICES > ARC, set the same
  way through `arc.vports[1]`.
- **Multi-touch events are not laid out as `types.h` says.** With two
  fingers on one view the plugin sends `events = 2, num_pointers = 1`: one
  row per finger, and `index` counts through the rows. Code that reads
  `pointers[0][index]` or only the last row sees a single finger. Walk all
  rows and match by pointer id (`tgui_event_pointer()`, `tgui_touches_move()`).
- **Touch coordinates on an image view with a buffer are in buffer
  pixels** (0..1023 x 0..511 here), whatever size the view is drawn at; on
  other views they are view pixels. Found by tapping the corner pads.
- Grid state: tried by hand with `awake`. Taps map to the right cells
  (corner pad = 16,8) and the script reacts, chords included. Page switching
  (swipe, encoder-zone tap, side strips out to the edges) has been used by
  hand too (before the arc page was added, which changed where the swipes
  and strips lead). Holds have not been looked at. The
  picture is sized from the window height reported with the system bars
  showing (354 dp on the S23+), so it may sit a little short of the bottom
  edge. `grid = false` in the options turns it off.
- The layout follows the rotation of the device, whatever the system
  auto-rotate setting says (`orientation = "portrait"` or `"landscape"`
  fixes it). On a rotation the plugin sends a CONFIG event and the views are
  rebuilt in the same window. Both layouts, and rotating between them with
  the window open, have been used by hand.
- `TGUI_ERR_MESSAGE` (4) is mostly not a broken connection: the library
  returns it when the plugin answers `success = false`, i.e. it did not
  carry out that one call. It showed up on about half of the cold starts,
  on an arbitrary call shortly after `tgui_activity_configure_insets()`,
  and the same call works a moment later. `TGUI_TRY` therefore repeats a
  call up to ten times, presenting a frame is simply tried again on the
  next one, and the insets are only touched when they have to change. A
  failed open is still retried twice as a last resort.
- Keys follow touch down/up, so holding K1 works. Encoders are drag pads:
  right or up is clockwise, one step per `enc_step_dp` (default 12) of
  travel. They post `EVENT_SDL_ENC`, i.e. exact steps with no acceleration.
- Options: `_boot.add_io('screen:tgui', {enc_step_dp = 12, keep_screen_on = true, debug = false, orientation = "auto", zone_width = 0.25, grid = true, arc = true})`.
- `tgui_get_dimensions()` returns `TGUI_ERR_MESSAGE` for a view that has
  not been laid out yet, so it is no use while building a layout. It works
  a few hundred ms later.
- Back hides the window. If it is closed (swiped from recents, or destroyed
  by Android), matron keeps running; `termux/show.sh` or
  `_norns.screen_tgui_show()` opens it again. For an open but backgrounded
  window the same call asks Android to raise its task (not yet tried; it
  should need the "Appear on top" permission below).
- If the plugin is missing or dies, the backend logs a `WARN (screen:tgui)`
  line and norns continues headless; it never fails IO setup.
- Two threads: one opens the window and presents frames (60 Hz, stopped by
  a flag); one blocks in `tgui_wait_event()`. Do not go back to
  `tgui_poll_event()`: it only checks the socket, not the library's read
  buffer, so a release that arrives with its press is held until the next
  event and short K1 taps never register. The event thread is stopped with
  `pthread_cancel` (repeated until it acknowledges), which makes its
  `read()` fail with `EINTR`.
- `debug = true` in the options logs window events and touches to
  `norns.log`.
- **Termux:GUI needs the "Appear on top" permission** (Settings -> Apps ->
  Termux:GUI -> Appear on top, or
  `am start -a android.settings.action.MANAGE_OVERLAY_PERMISSION`). Without
  it Android only lets the plugin open a window shortly after one of its
  windows was on screen; otherwise the open call blocks for good: no
  `window open` line in `norns.log`, and since the stuck thread holds the
  state lock `show.sh` does nothing until the stack is restarted. Seen three
  times before the permission was granted; the first cold start after
  granting it opened normally.
- Not done: the hardware keyboard / soft keyboard is not forwarded to the
  Lua `keyboard` module.

### SDL (`screen:sdl`)

Still compiled in, still never run. Termux's `sdl2` only has the `x11` and
`wayland` video drivers, so it needs an X server: the `termux-x11-nightly`
package is installed, but it is only a loader for the **Termux:X11** Android
app, which is distributed as an APK at
<https://github.com/termux/termux-x11/releases/tag/nightly> and is not
installed. With it: `termux-x11 :0 &`, `DISPLAY=:0`, and
`_boot.add_io('screen:sdl', {})`. If SDL fails at setup, matron exits and a
dead client takes JACK with it.

## Repo conventions

- This is the `vehka` GitHub account's fork; commits here must use the vehka
  identity (`git config user.email` → `vehka@iki.fi`, remote
  `git@github-vehka:vehka/norns`). Check both before committing.
- Keep Android changes behind `__ANDROID__` / `NORNS_ANDROID` so the Pi and
  desktop builds are unaffected.
