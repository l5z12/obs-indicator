# Changelog

All notable changes to obs-indicator are documented here. Versions follow [semantic versioning](https://semver.org):
a **major** release can change or remove settings, or drop support for an OBS or Windows version; a **minor** release
adds features; a **patch** release fixes bugs.

## 1.0.0 (2026-11-25)

### Features

#### First release

A ShadowPlay-style on-screen overlay for OBS Studio on Windows.

- Status indicator in a screen corner while the replay buffer, recording, stream or virtual camera is active,
  with optional elapsed times and a microphone icon (when muted, or always).
- Notifications for the replay buffer (with your save hotkey and buffer length), saved replays, recordings
  (started, paused, resumed, saved), streams (started, ended, duration), screenshots, the virtual camera and
  microphone mute. Saved files show their file name.
- Settings under **Tools → Overlay Indicator**: corner, display (including the display with the active window),
  size, opacity, margin, accent colour, which events show, and a live Preview.
- A "Toggle overlay indicator" hotkey.
- The overlay is hidden from screen capture by default, click-through, never takes focus, and doesn't inject
  into games.
- Installs in the OBS 33 plugin layout, with metadata for OBS's Plugin Manager.
