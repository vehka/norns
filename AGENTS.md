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
  cancel on script clear) work.
- matron and sclang REPLs work over ws-wrapper.

Not done / not tested:

- **SDL screen** (`screen:sdl`): compiled in, never run. Next thing to test.
- Audio input, softcut under load, MIDI, grid, crow, HID.
- `termux/build.sh` was written after the steps were done by hand. It has
  only been syntax-checked, not run end to end.

## Layout

| Path | What |
|---|---|
| `~/norns` | this checkout (the Lua side hardcodes `$HOME/norns`) |
| `~/dust` | scripts and data (`code`, `data`, `audio/tape`) |
| `~/matronrc.lua` | currently headless (no IO registered) |
| `~/norns-deps/prefix` | nng (static), libmonome, `jack_connect`, `jack_lsp`, `repl-send` |
| `~/norns-deps/waf` | waf 2.1.4; the bundled `./waf` fails on Python 3.12+ |
| `~/.local/share/SuperCollider/Extensions/norns-config.sc` | adds the norns SC class paths |
| `$TMPDIR/norns-run/` | `jack.log`, `sclang.log`, `norns.log` |

## Build and run

```sh
termux/build.sh     # packages, deps, configure, build, runtime layout
termux/start.sh     # jack -> sclang -> norns; waits for startup ok
termux/stop.sh      # stops everything, including jack
```

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

`build/maiden-repl/maiden-repl` is the interactive equivalent. The REPL
transport is nng bus0 over websocket in text mode (`ws4://`, with
`NNG_OPT_WS_SEND_TEXT` / `RECV_TEXT`); a plain websocket client will not work.

## Gotchas

- **Starting norns makes sound.** The startup chime plays on the phone
  speaker. To test an engine silently, send `audio.level_dac(0)` first, and
  restore with `params:lookup_param("output_level"):bang()`.
- **`pkill -f` kills your own shell** if the pattern appears anywhere in the
  command line you are running, heredocs included (exit code 144). Call
  `termux/stop.sh` on its own line rather than inlining `pkill -f` patterns.
- **`pkill -x norns` does not match.** norns and ws-wrapper are started by
  path, and the sidecar child renames itself to `sidecar [norns]` and ignores
  SIGTERM. `stop.sh` matches command lines and escalates to SIGKILL.
- **JACK does not survive a client crash.** After any client dies uncleanly,
  `jackd` refuses new clients (`jack_client_open() failed; status = 33`).
  Restart the whole stack; `start.sh` always does.
- **sclang must be up before norns**, and needs one throwaway start after
  `norns-config.sc` is first installed (the include paths are only compiled in
  on the next launch).
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
    `pthread_testcancel`. Emulated as deferred cancellation: `pthread_cancel`
    flags the thread and sends SIGUSR2 (empty handler, no `SA_RESTART`) so its
    blocking call returns `EINTR`; the thread exits at its next
    `pthread_testcancel()`. Force-included into every C++ unit via
    `norns/wscript`.
  - `device_monitor.cc`: no-op replacement for the udev monitor.
  - `dns_sd.h`: stub for avahi's compat header (no mDNS advertising).
- `wscript`, `norns/wscript`: detect `__ANDROID__` (`NORNS_ANDROID`); skip
  libudev, libgpiod, avahi and `watcher`; swap in the compat sources.
- `third-party/wscript`: two clang-only warning-flag fixes for lua-cjson.
- `matron/src/metro.cc`, `hardware/stat.cc`, `hardware/battery.cc`: extra
  `pthread_testcancel()` calls so the loops are cancellable under the shim.
- `norns/sidecar.cpp`: IPC socket path honours `$TMPDIR`.
- `crone/src/Client.h`: missing capture ports are a warning, not an abort.
- `matron/src/hardware/platform.cc`, `hardware/stat.cc`: tolerate unreadable
  device-tree and `/proc/stat`.
- `termux/`: `build.sh`, `start.sh`, `stop.sh`, `env.sh`, `repl-send.c`.

**Rule for new threads on Android:** blocking calls are not cancellation
points here. Any thread that something will `pthread_cancel` (and especially
`pthread_join` afterwards, as `dev_delete()` does) must call
`pthread_testcancel()` in its loop after each blocking call, or the join hangs.

## Known limits

- **No audio input.** `jackd -d opensles` comes up playback-only, and
  `-C 2` makes the driver fail to initialise. Probably the Termux app lacks
  the microphone permission (Android Settings → Apps → Termux → Permissions);
  unconfirmed.
- **Latency.** 48 kHz, 960-frame periods, about 40 ms output latency, no
  real-time priority. A few xruns at startup.
- **No hardware devices** (USB MIDI, HID, grid, crow) without root. The plan
  is OSC control, TouchOSC for grid emulation, and Bluetooth MIDI from the
  `feat/bl-midi` branch (8 commits ahead of `main`, not merged here; its
  device threads need the `pthread_testcancel()` rule above).
- **Pi-only menus** (wifi, update, password) call `nmcli`, `systemctl`,
  `sudo` and just log errors.
- **Engines.** Only the stock SuperCollider UGens are installed; sc3-plugins
  is not packaged for Termux. The SC plugin headers are present
  (`$PREFIX/include/SuperCollider`), so it can be built.
- **Android may kill background processes.** `start.sh` takes a
  `termux-wake-lock`; `stop.sh` releases it.

## Next: the SDL screen

Not attempted yet. What is known going in:

- `sdl2` 2.32.10 is installed from `x11-repo` and the SDL backend
  (`matron/src/hardware/screen/sdl.cc`, `hardware/input/sdl.cc`) is compiled
  into the binary.
- An X server is needed. The usual route is the **Termux:X11** Android app
  plus the `termux-x11-nightly` package (available in `x11-repo`, not
  installed); neither was present at the time of writing. Start the server,
  export `DISPLAY` (typically `:0`) in the environment `start.sh` runs in.
- Enable the window by copying `matronrc.lua.desktop` to `~/matronrc.lua`
  (minimal form: `_boot.add_io('screen:sdl', {})`). It opens a 512x256 window
  mirroring the 128x64 screen.
- Keys and encoders are keyboard chords (Alt+1/2/3, Alt+q/w a/s z/x), which
  is awkward on a touch keyboard; the `input = {...}` table in
  `matronrc.lua.desktop` can remap them, and `modifier = 'none'` removes the
  need for Alt.
- If SDL fails at setup, matron exits (`io_setup_all` returns the error), and
  a dead client takes JACK with it — restart the stack after each attempt.

## Repo conventions

- This is the `vehka` GitHub account's fork; commits here must use the vehka
  identity (`git config user.email` → `vehka@iki.fi`, remote
  `git@github-vehka:vehka/norns`). Check both before committing.
- Keep Android changes behind `__ANDROID__` / `NORNS_ANDROID` so the Pi and
  desktop builds are unaffected.
