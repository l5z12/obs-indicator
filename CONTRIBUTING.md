# Contributing

Bug reports, ideas and pull requests are welcome. For anything larger than a small fix, please open an issue
first so we can agree on the approach.

## Reporting a bug

Use the bug report form. The most useful thing you can attach is your OBS log: in OBS, choose
**Help → Log Files → Upload Current Log File** and paste the URL. Lines starting with `[obs-indicator]` come
from this plugin.

## Building

You need Windows 10 or 11 (x64), Visual Studio 2022 or newer with *Desktop development with C++*, and Git.
You don't need to build OBS or install Qt.

```powershell
.\scripts\build.ps1                      # uses your installed OBS (C:\Program Files\obs-studio)
.\scripts\build.ps1 -Install             # also installs into OBS (close OBS first)
```

Without OBS installed, download the release files the build needs first (this is what CI does):

```powershell
.\scripts\fetch-obs-binaries.ps1 -Version 33.0.3
.\scripts\build.ps1 -ObsDir deps\obs-33.0.3
```

`build.ps1` also takes `-Package`, which writes the release zips to `build\dist\`, and `-WarningsAsErrors`.

## Trying changes without OBS

Most of the plugin is the overlay itself, which has no OBS dependency:

```powershell
build\overlay-demo.exe                   # plays a scripted session with the real overlay windows
build\overlay-demo.exe --corner bl       # ...in another corner (tl, tr, bl, br)
build\overlay-demo.exe --dialog          # the settings dialog, with a working Preview
build\overlay-preview.exe art.png 2      # every indicator state, notification and icon, rendered at 2x
```

The overlay is hidden from screen capture, so screenshots of it come out empty. Add `--capturable` to the demo,
or untick the setting, when you need a screenshot.

## Code guidelines

- The plugin uses only the OBS C API and Win32: no Qt, no injection into other processes.
- C++20. The build uses `/W4` and must stay free of warnings (CI builds with `-WarningsAsErrors`).
- Formatting follows OBS Studio: tabs shown 8 wide, lines up to 120 columns (see `.editorconfig`).
- OBS callbacks can arrive on any thread. Hand work to the OBS UI thread with `obs_queue_task`, and pass state
  to the overlay only through `Overlay`'s thread-safe setters.
- Notification text belongs in `data/locale/en-US.ini`, not in code.
- The README's *How it works* section describes how the files fit together.

## Documenting changes

The changelog and version numbers come from *change files*, which [Knope](https://knope.tech) turns into releases.
Commit messages aren't used, so write them however you like.

If your pull request changes something users will notice, add a Markdown file to `.changeset/`. Name it after
the change, for example `.changeset/fix_toast_flicker.md`:

```markdown
---
default: patch
---

# Fix toasts flickering on a second monitor

Optional details, in as much Markdown as you like.
```

- `default` is the package name Knope uses for a single-package repo. Its value is the type of change:
  - **patch** for fixes;
  - **minor** for new features or settings;
  - **major** for changes that break existing setups, for example dropping support for an OBS version.
- The `#` heading becomes the changelog entry. Write it, and any details, for users, because they go into
  `CHANGELOG.md` and the release notes as written.
- Internal changes (CI, refactoring, docs) don't need a change file.

If you have [Knope installed](https://knope.tech/installation/), `knope document-change` asks for the type and
summary and writes the file for you.

Don't edit `CHANGELOG.md` or the version in `CMakeLists.txt` by hand.

## Releasing (maintainers)

1. When change files reach `main`, CI opens a **Release obs-indicator x.y.z** pull request from the `release`
   branch, or updates the one that's already open. It bumps the version in `CMakeLists.txt`, adds the new
   section to `CHANGELOG.md`, and deletes the change files.
2. When you're ready to release, merge that pull request.
3. CI then builds the plugin, creates the `vX.Y.Z` tag, and publishes a GitHub release. The release notes are
   the new changelog section, and the zips are attached.

For this to work, enable **Settings → Actions → General → Allow GitHub Actions to create and approve pull
requests**. Pull requests opened by GitHub Actions don't trigger other workflows, so the release pull request has
no build check of its own. The build runs again when it's merged.

To publish a version that's already in `CMakeLists.txt` but has no tag yet, run the Build workflow by hand from
the Actions tab, on `main`. This is how 1.0.0 is published: the repository starts with 1.0.0 prepared. (From a
0.x version Knope never picks 1.0.0 by itself, so preparing it in advance avoids that.)

The version in `CMakeLists.txt` also sets the DLL's version info, `manifest.json`, and the zip names from
`build.ps1 -Package`. CI installs the Knope version set as `KNOPE_VERSION` in the workflow.

To build against a newer OBS, update `OBS_VERSION` and `OBS_SHA256` in `.github/workflows/build.yml`. The
digest of the release zip is printed by:

```powershell
gh api repos/obsproject/obs-studio/releases/tags/<version> --jq '.assets[] | select(.name | endswith("-Windows-x64.zip")) | .digest'
```

## License

By contributing, you agree that your contributions are licensed under GPL-2.0-or-later, the same license as the
project.
