# obs-indicator

[![Latest release](https://img.shields.io/github/v/release/l5z12/obs-indicator)](https://github.com/l5z12/obs-indicator/releases/latest)
[![License](https://img.shields.io/badge/license-GPL--3.0--or--later-blue)](LICENSE)

A ShadowPlay-style overlay for OBS Studio on Windows. A small indicator shows while you're recording, streaming or
running the replay buffer, and a notification pops up when something happens, like "Replay saved".

![The overlay over a game](docs/overlay.png)

The overlay never appears in your recordings or stream, never takes focus, and doesn't hook into games, so
anti-cheat has nothing to complain about. The one exception is the optional *Draw inside OpenGL games*: it hooks
into the game, and Display Capture and Window Capture then record the overlay.

## Install

You need Windows 10 (version 2004 or newer) or Windows 11, and 64-bit OBS Studio 33 or newer.

Open PowerShell and run:

```powershell
irm https://raw.githubusercontent.com/l5z12/obs-indicator/main/install.ps1 | iex
```

This downloads the latest release, checks it against the SHA-256 that GitHub lists for it, and installs it to
`C:\ProgramData\obs-studio\plugins\obs-indicator`. If OBS has the plugin loaded, it waits for you to close OBS. Run
the same line again to update.

To install by hand instead:

1. Download `obs-indicator-<version>-windows-x64.zip` from the
   [latest release](https://github.com/l5z12/obs-indicator/releases/latest).
2. Close OBS (and any game the overlay draws inside) and extract the zip into `C:\ProgramData\obs-studio\plugins\`.
3. Start OBS.

To uninstall, delete `C:\ProgramData\obs-studio\plugins\obs-indicator`.

## Use

Open **Tools → Overlay Indicator** to choose what shows, where, and how big. **Preview** shows the result.
To turn the overlay on and off while you play, set a key for **Toggle overlay indicator** in
**Settings → Hotkeys**.

![The settings dialog](docs/settings.png)

## Troubleshooting

- **The overlay doesn't show over a game.** The game is probably using exclusive fullscreen, where its picture goes
  straight to the screen and no window can show over it. Switch the game to borderless or windowed fullscreen.
- **Minecraft (or another OpenGL game) hides the overlay in fullscreen.** Tick *Draw inside OpenGL games* under *Show
  the overlay over fullscreen games*. This is for Minecraft's OpenGL renderer (before 26.4, or with *Graphics API* set
  to OpenGL); with its Vulkan renderer, *Show the overlay over fullscreen games* alone is enough. The overlay is then drawn into the game's own picture by a small helper loaded
  into the game, so it's off by default: don't use it with games that have anti-cheat. OBS's Game Capture still
  leaves the overlay out, but Display Capture and Window Capture record it.
- **The game is borderless or windowed but still covers the overlay.** Some games present with flip-model or
  independent flip, which can push a normal always-on-top window behind them. Tick *Show the overlay over fullscreen
  games (advanced)* in the settings: it raises the overlay into a higher window layer with
  [WindowTopMost](https://github.com/xmc0211/WindowTopMost). This injects a helper into Explorer (not the game), so
  it's off by default, and it can't help with the exclusive fullscreen above, which bypasses the desktop compositor.
  The first time the helper starts after you sign in, focus may jump to the taskbar and back, or the Start menu may
  open and close. Start OBS before the game so this doesn't interrupt it.
- **You want the overlay in your recording.** Untick *Hide the overlay from screen capture* in the settings.
- **The mic icon ignores push-to-talk.** It follows the mic's mute state only.
- **Something else.** [Open an issue](https://github.com/l5z12/obs-indicator/issues/new/choose) and attach your
  OBS log (**Help → Log Files → Upload Current Log File**).

## License

GPL-3.0-or-later. Want to help? See [CONTRIBUTING.md](CONTRIBUTING.md).
