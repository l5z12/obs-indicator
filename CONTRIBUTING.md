# Contributing

Issues and pull requests are welcome. For bigger changes, open an issue first.

## Build

You need Visual Studio 2022 or newer with *Desktop development with C++*, and Git.

```powershell
.\scripts\build.ps1             # builds against your installed OBS
.\scripts\build.ps1 -Install    # ...and installs it (close OBS first)
build\overlay-demo.exe          # runs the overlay without OBS (--dialog opens the settings)
```

No OBS installed? Run `.\scripts\fetch-obs-binaries.ps1 -Version 33.0.3` first, then build with
`-ObsDir deps\obs-33.0.3`.

## Code map

- `src/plugin-main.cpp`: the OBS side (events, hotkey, settings, Tools menu)
- `src/overlay.cpp`, `src/renderer.cpp`: the overlay windows and drawing, on their own thread
- `src/settings-dialog.cpp`, `src/obs-indicator.rc`: the settings dialog
- `tools/`: demo and preview programs that run without OBS

## Rules

- OBS C API and Win32 only: no Qt, no hooking into games.
- OBS callbacks can run on any thread. Use `obs_queue_task` to get onto the UI thread.
- No compiler warnings, and OBS code style (tabs, 120 columns).
- Commit messages and PR titles use [Conventional Commits](https://www.conventionalcommits.org):
  `fix: …`, `feat: …`, `docs: …`.

## Changelog

If users will notice your change, add a file to `.changeset/`, for example `.changeset/fix_toast_flicker.md`:

```markdown
---
default: patch
---

# Fix toasts flickering on a second monitor
```

Use `patch` for fixes, `minor` for features and `major` for breaking changes. The heading goes into the changelog
as written. [docs/releasing.md](docs/releasing.md) covers how releases are made.

By contributing, you agree to license your work under GPL-2.0-or-later.
