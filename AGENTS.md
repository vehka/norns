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
| `$TMPDIR/norns-run/` | `jack.log`, `aaudio.log`, `sclang.log`, `norns.log` |

## Build and run

```sh
termux/build.sh     # packages, deps, configure, build, runtime layout
termux/start.sh     # jack -> jack-aaudio -> sclang -> norns; waits for startup ok
termux/stop.sh      # stops everything, including jack
termux/show.sh      # reopen the norns window after closing it
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
- `termux/`: `build.sh`, `start.sh`, `stop.sh`, `show.sh`, `env.sh`,
  `repl-send.c`, `jack-aaudio.c`.

**Rule for new threads on Android:** blocking calls are not cancellation
points here. Any thread that something will `pthread_cancel` (and especially
`pthread_join` afterwards, as `dev_delete()` does) must call
`pthread_testcancel()` in its loop after each blocking call, or the join hangs.

## Known limits

- **No audio input.** The dummy driver's `system:capture_*` ports are
  silent. (`jackd -d opensles` came up playback-only, and `-C 2` made it
  fail to initialise; probably Termux lacks the microphone permission,
  unconfirmed.) An AAudio input stream in `jack-aaudio` would be the way in.
- **Latency.** 48 kHz, 960-frame periods plus a 60 ms output buffer (see
  Audio output), no real-time priority.
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

## Audio output

`start.sh` runs `jackd -d dummy -m` (a timer-driven server with monitor
ports) and `termux/jack-aaudio.c`, a JACK client that copies
`system:monitor_*` into a ring buffer played by an AAudio callback stream.
crone still connects to `system:playback_*` as on any norns.

- Why: Termux's own driver (`jackd -d opensles`) crackles. It keeps two
  OpenSL buffers queued, so one late cycle is an audible dropout that JACK
  does not count as an xrun, and JACK cycles here are regularly late (no
  real-time scheduling; 25-80 ms gaps seen against a 20 ms period).
  `NORNS_JACK_DRIVER=opensles termux/start.sh` goes back to it.
- `NORNS_AUDIO_BUFFER_MS` (default 60) is the ring's target fill, i.e. how
  late a cycle may be; `NORNS_JACK_PERIOD` (default 960) is the JACK period.
- `aaudio.log` gets a line whenever the underrun/overrun counters change.
  An underrun plays silence until the ring is back at the target. Overruns
  are the slow drift between the timer and the audio device being corrected
  by skipping ahead. Seen: a few underruns while a script loads, none in 45 s
  of `awake` playing. Not yet judged by ear.
- It reopens the stream when Android reports an error (output device
  change); not exercised.
- `jack_frame_time()` is stuck at 0 with this driver too, hence the
  `CLOCK_MONOTONIC` change in `jack_client.cpp`.

## The screen: Termux:GUI

`screen:tgui` shows norns in a window of the **Termux:GUI** app
(`com.termux.gui`, same F-Droid source as Termux) through `libtermuxgui`
(`pkg install termux-gui-c`). No X server is involved.

- The window is portrait: the screen on top (128x64 upscaled by an integer
  factor into a shared buffer, 1024x512 on the S23+), then touch pads in the
  hardware arrangement: `K1 E1` / `E2 E3` / `K2 K3`.
- Keys follow touch down/up, so holding K1 works. Encoders are drag pads:
  right or up is clockwise, one step per `enc_step_dp` (default 12) of
  travel. They post `EVENT_SDL_ENC`, i.e. exact steps with no acceleration.
- Options: `_boot.add_io('screen:tgui', {enc_step_dp = 12, keep_screen_on = true, debug = false})`.
- Back hides the window. If it is closed (swiped from recents, or destroyed
  by Android), matron keeps running; `termux/show.sh` or
  `_norns.screen_tgui_show()` opens it again. An open but backgrounded
  window cannot be raised from here; use the recents list.
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
  Lua `keyboard` module, and there is no landscape layout.

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
