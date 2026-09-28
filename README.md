# typeless-omarchy

An unofficial arm64 Linux port of [Typeless](https://www.typeless.com), the AI
voice-dictation app, for Omarchy / Hyprland on aarch64 machines.

Typeless only ships an x64 `.deb` for Linux. The app is Electron, so most of it
runs anywhere once its runtime is swapped, but it loads six closed-source x64
native helpers in-process. On a 16K-page kernel (Apple silicon) x86 emulation
isn't an option, so this repo contains **clean-room arm64 reimplementations**
of those helpers, written against the behaviour of the originals, plus the
scripts that turn the official download into a working arm64 install.

**Nothing from Typeless is in this repo.** `build.sh` downloads the official
package from Typeless's own update feed at build time (verified against the
feed's sha512) and assembles the port locally. Not affiliated with Typeless.

## Quick start

```bash
./build.sh            # compile helpers, download Typeless, assemble stage/Typeless
./install.sh --all    # app + Hyprland rules + mic-button watcher
```

Then open Typeless from the launcher and sign in.

`./install.sh` without flags installs only the app. `--hyprland` adds the
window rules, `--mic-button[=<source>]` adds the USB-mic button watcher.
`./uninstall.sh` removes everything (`--purge` also deletes
`~/.config/Typeless`).

### Requirements

- aarch64 Linux with Wayland (tested: Omarchy, Hyprland 0.56, PipeWire)
- Build: `base-devel`, `pkg-config`, `libpulse`, `libsecret`, `glib2`,
  `at-spi2-core`, `dbus`, `wayland`, `wayland-protocols`, `libarchive`
  (`bsdtar`), `curl`, `python3`, Node.js 20+ (`npx`)
- Runtime: `wl-clipboard`, `at-spi2-core`, XWayland
- Read access to `/dev/input/event*` and write access to `/dev/uinput`
  (logind's `uaccess` ACLs or the `input` group)

## What gets built

| Piece | Source | Notes |
|---|---|---|
| Electron runtime | electron/electron releases | Same version as upstream (detected from the `.deb`), fuses mirrored |
| `better-sqlite3` | WiseLibs prebuilds | Matched to the Electron ABI |
| `koffi` | bundled in the app | Its `linux_arm64` build is already in `app.asar` |
| `libKeyboardHelper.so` | `helpers/keyboard-helper` | Global hotkeys from evdev |
| `libInputHelper.so` + `typeless-clipboard` | `helpers/input-helper` | Text insertion: clipboard + Ctrl+V (Ctrl+Shift+V in terminals) through a uinput keyboard, clipboard restored afterwards |
| `libContextHelper.so` | `helpers/context-helper` | Focused app (Hyprland IPC), focused element / surrounding text / URL (AT-SPI) |
| `libUtilHelper.so` | `helpers/util-helper` | Audio devices, ducking, sounds, device id, permission checks |
| `libopusenc.so` | `helpers/libopusenc` | libopus 1.5.2 + libopusenc 0.2.1 + the app's wrapper API |
| `typeless-atspi-helper` | `helpers/linux-shared` | Shared out-of-process AT-SPI helper |

Each helper has its own `build.sh` and headless tests under `test/`.

`patches/` edits the app bundle:

- `10-disable-updater.sh` — the upstream feed only has x64 packages, so the
  in-app updater always reports "up to date". To update, run
  `./build.sh <new-version>` and `./install.sh`.
- `20-force-x11.sh` — the floating bar positions itself with raw Xlib calls, so
  the app must run under XWayland. Omarchy sets
  `ELECTRON_OZONE_PLATFORM_HINT=wayland` and Typeless rewrites its own launcher
  entries, so the app relaunches itself once with `--ozone-platform=x11`
  (`TYPELESS_ALLOW_WAYLAND=1` disables this).

## Differences from upstream behaviour

- **Hotkeys don't grab the keyboard.** Upstream takes exclusive control of every
  keyboard (EVIOCGRAB) so shortcut keys don't reach apps; that breaks other
  hotkey readers such as Wispr Flow. This port listens passively by default;
  `KEYBOARD_HELPER_NO_GRAB=0` restores grabbing.
- **Non-keyboard buttons work.** Upstream only watches full keyboards; this port
  also reads media/consumer-key devices (headset buttons, remotes) and names
  keys it doesn't know (e.g. `VoiceCommand`, `Fn`).
- **Accessibility check** asks the AT-SPI bus directly instead of going through
  the helper process, which deadlocks while a Typeless window has focus.

## Mic mute button → dictation (`extras/mic-button`)

Some USB mics (e.g. the JIAYZ BY-GM18CU) have a button that toggles a
*hardware* mute and reports nothing to the computer: no key event, no mixer
change. A muted mic streams exact digital zeros, while a live one always has a
noise floor, so `typeless-mic-button` listens to the mic and taps a virtual
`VoiceCommand` key whenever the state flips:

- unmute (LED live) → tap → Typeless starts dictating
- mute → tap → Typeless finishes and pastes

Setup: `./install.sh --mic-button=<source>` (find the source with
`pactl list short sources`; the default is the BY-GM18CU), then in Typeless
set the **Dictate** shortcut by pressing the mic button once.

The key and behaviour are set in `~/.config/typeless-mic-button.env`
(restart with `systemctl --user restart typeless-mic-button`):

| App | `MIC_BUTTON_KEY` | `MIC_BUTTON_MODE` | Bind in the app |
|---|---|---|---|
| Typeless (default) | `voicecommand` | `tap` | Dictate → press the button once |
| Wispr Flow | `f24` | `hold` | Push-to-talk → unmute, then mute |

Wispr Flow only reads devices that have letter keys and has no
`VoiceCommand` key code, so for Wispr the watcher sends F24 and holds it
while the mic is live (push-to-talk).

- The virtual device is disabled in Hyprland (`config/hypr/typeless.lua`) so
  the key never reaches apps; Typeless reads `/dev/input` directly.
- The watcher keeps a capture stream open, so Omarchy's bar microphone icon
  always shows "in use". The mic's LED is the real indicator.
- It follows the mute state, not Typeless's state: if dictation ends another
  way, the next mute starts one. Press the button again to resync.

## Troubleshooting

- **Orange border / box over the screen:** the floating bar window is missing
  its rules — run `./install.sh --hyprland` and restart Typeless.
- **App exits at start with `BadWindow`:** it's running on native Wayland; check
  that `patches/20-force-x11.sh` was applied (rebuild).
- **Logs:** start `~/.local/opt/Typeless/typeless` from a terminal; the helpers
  print to stderr. Mic watcher: `journalctl --user -u typeless-mic-button`.
