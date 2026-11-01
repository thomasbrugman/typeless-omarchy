# typeless-omarchy

> **Fork status.** Upstream describes this port as shelved, but it still builds:
> `./build.sh` now reads the current release from Typeless's update feed and
> verifies the `.deb` against the feed's sha512, so it tracks the latest version
> without edits. The XWayland patch is also version-agnostic, unlike upstream's
> which pinned the 2.2.0 entry chunk name. Verified building 2.2.1 on
> aarch64/Omarchy (Hyprland 0.57). Pin a release with `./build.sh 2.2.1` or
> `TYPELESS_VERSION=2.2.1`; if a future release changes the bundle layout the
> patches fail loudly rather than shipping a half-ported app.
>
> **Upstream status (2026-10-02).** Typeless is not installed; Wispr Flow is the
> dictation app in use. Build outputs (`work/`, `stage/`, `dl/`) have been removed;
> rebuild with the instructions below (`./build.sh`) if this is ever picked up again.

An unofficial arm64 Linux port of [Typeless](https://www.typeless.com), the AI
voice-dictation app, for Omarchy / Hyprland on aarch64 machines.

Typeless only ships an x64 `.deb` for Linux. The app is Electron, so most of it
runs anywhere once its runtime is swapped, but it loads six closed-source x64
native helpers in-process. On a 16K-page kernel (Apple silicon) x86 emulation
isn't an option, so this repo contains **interoperable arm64
reimplementations** of those helpers (behaviour reconstructed from the x64
binaries for interoperability), plus the scripts that turn the official
download into a working arm64 install.

**No Typeless code or binaries are in this repo.** `build.sh` downloads the official
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

## Prebuilt releases

[`.github/workflows/release.yml`](.github/workflows/release.yml) checks Typeless's
update feed every 6 hours and, when a version it has not published yet appears,
builds it on an aarch64 runner and publishes a release with **the same version
number Typeless uses** (upstream `3.0` → release tag `3.0`). The `.deb` is still
fetched from the vendor and checked against the feed's sha512; only the arm64
runtime and helpers are ours.

That builds on Typeless's code without anyone watching, so treat a red run as
"upstream changed something": the patches in `patches/` assert on the bundle
layout they expect and fail loudly rather than shipping a half-ported app.

Installing a published build means no toolchain and no compile:

```bash
gh release download 2.2.1 --pattern '*.tar.zst' --pattern SHA256SUMS   # omit version for latest
sha256sum -c SHA256SUMS
tar --zstd -xf Typeless-2.2.1-arm64-omarchy.tar.zst
cd Typeless-2.2.1-arm64-omarchy
./install.sh --hyprland
```

## Install with pacman

Each release is also repackaged as an Arch package and published as a pacman
repository, so it installs and upgrades like any other package. Add the
repository once:

```bash
sudo tee -a /etc/pacman.conf >/dev/null <<'EOF'

[typeless-omarchy]
SigLevel = Optional TrustAll
Server = https://thomasbrugman.github.io/typeless-omarchy
EOF

sudo pacman -Sy
omarchy pkg add typeless-omarchy-bin
```

Upgrades then arrive with `omarchy update` or `pacman -Syu`. The package
installs `/opt/Typeless`, `/usr/bin/typeless`, a desktop entry, and udev rules
that grant the seat user access to `/dev/input/event*` and `/dev/uinput`, so no
group changes or config edits are needed. The floating-bar rules ship in
`/usr/share/typeless-omarchy/hypr/typeless.lua`; the post-install message shows
how to enable them.

Packages are unsigned and the server is HTTPS-only (`TrustAll` above is what
lets pacman accept them). Signing the repository with a key held in CI is the
obvious improvement if that trust model is too loose. `typeless-bin` from the
AUR conflicts, so remove it first — pacman will do it for you on install.

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
| Wispr Flow | `f24` + `MIC_BUTTON_MODIFIER=rightctrl` | `hold` | Push-to-talk → unmute, then mute |

Wispr Flow only reads devices that have letter keys, has no `VoiceCommand`
key code and insists on a modifier, so for Wispr the watcher holds
Right Ctrl + F24 while the mic is live (push-to-talk). Its helper also only
opens input devices that exist when it starts: start Wispr after the watcher
(or restart Wispr after restarting the watcher).

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

## License

MIT for the scripts and helper sources in this repository; see
[LICENSE](LICENSE). Typeless itself is proprietary, is not included, and is
downloaded from its vendor at build time under its own terms. This is an
unofficial project, not affiliated with or endorsed by Typeless.
